#!/usr/bin/env bash
# shellcheck disable=SC2016
set -euo pipefail

repository=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
temporary=$(mktemp -d /tmp/sanguinius-deployment-test.XXXXXXXX)
cleanup() {
  find "$temporary" -type f -delete
  find "$temporary" -type l -delete
  find "$temporary" -depth -type d -empty -delete
}
trap cleanup EXIT

for script in "$repository/scripts/deploy_nuln.bash" \
              "$repository/scripts/lib/remote_deploy.bash"; do
  bash -n "$script"
done

snapshot=$(sha256sum "$repository/scripts/lib/remote_deploy.bash")
if "$repository/scripts/lib/remote_deploy.bash" invalid-operation \
    --expected-helper-sha "${snapshot%% *}" >/dev/null 2>&1; then
  echo "remote helper accepted an invalid operation" >&2
  exit 1
fi

if "$repository/scripts/deploy_nuln.bash" deploy --archive relative \
    --expected-schema 13 --target-schema 16 >/dev/null 2>&1; then
  echo "local deploy accepted a relative archive" >&2
  exit 1
fi

mkdir "$temporary/bin"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf '\''ssh %s\n'\'' "$*" >>"$SANGUINIUS_FAKE_SSH_LOG"' \
  'if [[ $* == *'\''mktemp -d /tmp/sanguinius-upload.XXXXXXXX'\''* ]]; then' \
  '  echo /tmp/sanguinius-upload.ABCDEFGH' \
  'fi' >"$temporary/bin/ssh"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf '\''scp %s\n'\'' "$*" >>"$SANGUINIUS_FAKE_SSH_LOG"' \
  'exit 0' >"$temporary/bin/scp"
chmod 0755 "$temporary/bin/ssh" "$temporary/bin/scp"
export SANGUINIUS_FAKE_SSH_LOG="$temporary/ssh.log"
PATH="$temporary/bin:$PATH" "$repository/scripts/deploy_nuln.bash" inspect \
  >/dev/null
grep -Fq 'ssh nuln set -eu; test "$(hostname -s)" = nuln' \
  "$SANGUINIUS_FAKE_SSH_LOG"
if grep -Fq 'sudo' "$SANGUINIUS_FAKE_SSH_LOG"; then
  echo "read-only inspection unexpectedly requested sudo" >&2
  exit 1
fi

PATH="$temporary/bin:$PATH" "$repository/scripts/deploy_nuln.bash" \
  rollback-same-schema --release-id 2.2.0-rollback-drill >/dev/null
grep -Fq 'sudo /bin/bash /tmp/sanguinius-upload.ABCDEFGH/remote_deploy.bash rollback-same-schema' \
  "$SANGUINIUS_FAKE_SSH_LOG"
grep -Fq 'scp ' "$SANGUINIUS_FAKE_SSH_LOG"

remote="$repository/scripts/lib/remote_deploy.bash"
SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-archive-entry \
  sanguinius-test/bin/sanguinius
if SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-archive-entry \
    sanguinius-test/../credential >/dev/null 2>&1; then
  echo "remote archive policy accepted traversal" >&2
  exit 1
fi
SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-payload-path \
  migrations/0016_cross_feature_reliability.sql
if SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-payload-path \
    config/bot.token >/dev/null 2>&1; then
  echo "remote payload policy accepted a credential path" >&2
  exit 1
fi
SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-payload-directory assets/vox
if SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-payload-directory cache \
    >/dev/null 2>&1; then
  echo "remote payload policy accepted an unmanaged directory" >&2
  exit 1
fi

candidate_release="$temporary/candidate-release"
candidate_credentials="$temporary/candidate-credentials"
mkdir -p "$candidate_release/bin" "$candidate_release/config" \
  "$candidate_release/assets/vox" "$candidate_credentials"
printf 'sentinel\n' >"$candidate_credentials/discord-token"
printf 'sentinel\n' >"$candidate_credentials/openai-key"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'set -euo pipefail' \
  '[[ ${1:-} == --check-config ]]' \
  '[[ $SANGUINIUS_TOKEN_FILE == "$CREDENTIALS_DIRECTORY/discord-token" ]]' \
  '[[ $SANGUINIUS_OPENAI_API_KEY_FILE == "$CREDENTIALS_DIRECTORY/openai-key" ]]' \
  '[[ -r $SANGUINIUS_TOKEN_FILE && -r $SANGUINIUS_OPENAI_API_KEY_FILE ]]' \
  >"$candidate_release/bin/sanguinius"
chmod 0755 "$candidate_release/bin/sanguinius"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'set -euo pipefail' \
  '[[ " $* " == *" --property=CacheDirectory=sanguinius "* ]]' \
  '[[ " $* " == *" --property=CacheDirectoryMode=0700 "* ]]' \
  'while (( $# )); do' \
  '  case "$1" in' \
  '    --unit|--uid|--gid|--working-directory) shift 2 ;;' \
  '    --quiet|--wait|--collect|--pipe|--service-type=*|--property=*) shift ;;' \
  '    *) break ;;' \
  '  esac' \
  'done' \
  'CREDENTIALS_DIRECTORY="$SANGUINIUS_FAKE_CREDENTIAL_DIRECTORY" "$@"' \
  >"$temporary/bin/systemd-run"
chmod 0755 "$temporary/bin/systemd-run"
export SANGUINIUS_FAKE_CREDENTIAL_DIRECTORY="$candidate_credentials"
PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true \
  SANGUINIUS_TEST_ROOT="$temporary" "$remote" policy-candidate-config \
  "$candidate_release"
rm "$temporary/bin/systemd-run"

mkdir "$temporary/payload"
printf '{}\n' >"$temporary/payload/RELEASE-METADATA.json"
(cd "$temporary/payload" && sha256sum ./RELEASE-METADATA.json \
  >SHARE-MANIFEST.sha256)
SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-payload-tree \
  "$temporary/payload"
mkdir "$temporary/payload/docs"
printf 'unlisted\n' >"$temporary/payload/docs/OPERATIONS.md"
if SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-payload-tree \
    "$temporary/payload" >/dev/null 2>&1; then
  echo "remote payload policy accepted an unlisted file" >&2
  exit 1
fi
rm -f "$temporary/payload/docs/OPERATIONS.md"
rmdir "$temporary/payload/docs"

mkdir -p "$temporary/system-release/sysusers.d" \
  "$temporary/system-release/tmpfiles.d"
printf 'u sanguinius - test\n' \
  >"$temporary/system-release/sysusers.d/sanguinius.conf"
printf 'd /var/lib/sanguinius 0750 sanguinius sanguinius -\n' \
  >"$temporary/system-release/tmpfiles.d/sanguinius.conf"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-system-configuration "$temporary/system-release"
[[ -d $temporary/etc/sysusers.d && -d $temporary/etc/tmpfiles.d ]]
[[ $(stat -c %a "$temporary/etc/sysusers.d") == 755 ]]
cmp "$temporary/system-release/sysusers.d/sanguinius.conf" \
  "$temporary/etc/sysusers.d/sanguinius.conf"
cmp "$temporary/system-release/tmpfiles.d/sanguinius.conf" \
  "$temporary/etc/tmpfiles.d/sanguinius.conf"

configuration_snapshot="$temporary/system-configuration.snapshot"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-system-configuration-snapshot "$configuration_snapshot"
printf 'u sanguinius - candidate\n' \
  >"$temporary/etc/sysusers.d/sanguinius.conf"
printf 'd /var/lib/sanguinius 0700 root root -\n' \
  >"$temporary/etc/tmpfiles.d/sanguinius.conf"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-system-configuration-restore "$configuration_snapshot"
cmp "$temporary/system-release/sysusers.d/sanguinius.conf" \
  "$temporary/etc/sysusers.d/sanguinius.conf"
cmp "$temporary/system-release/tmpfiles.d/sanguinius.conf" \
  "$temporary/etc/tmpfiles.d/sanguinius.conf"

rm "$temporary/etc/tmpfiles.d/sanguinius.conf"
absent_configuration_snapshot="$temporary/absent-system-configuration.snapshot"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-system-configuration-snapshot \
  "$absent_configuration_snapshot"
printf 'u sanguinius - second-candidate\n' \
  >"$temporary/etc/sysusers.d/sanguinius.conf"
printf 'd /var/lib/sanguinius 0755 root root -\n' \
  >"$temporary/etc/tmpfiles.d/sanguinius.conf"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-system-configuration-restore \
  "$absent_configuration_snapshot"
cmp "$temporary/system-release/sysusers.d/sanguinius.conf" \
  "$temporary/etc/sysusers.d/sanguinius.conf"
[[ ! -e $temporary/etc/tmpfiles.d/sanguinius.conf ]]

mkdir -p "$temporary/unit-release/systemd" "$temporary/unit-install"
printf '%s\n' \
  '{"service_unit":"sanguinius.service"}' \
  >"$temporary/unit-release/RELEASE-METADATA.json"
for unit in sanguinius.service sanguinius-backup.service \
    sanguinius-backup.timer; do
  printf 'unit=%s\n' "$unit" >"$temporary/unit-release/systemd/$unit"
done
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-release-units "$temporary/unit-release" \
  "$temporary/unit-install"
for unit in sanguinius.service sanguinius-backup.service \
    sanguinius-backup.timer; do
  cmp "$temporary/unit-release/systemd/$unit" \
    "$temporary/unit-install/$unit"
done
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    SANGUINIUS_TEST_FAIL_UNIT_INSTALL=sanguinius-backup.service \
    "$remote" policy-release-units "$temporary/unit-release" \
    "$temporary/unit-install" >/dev/null 2>&1; then
  echo "release unit installation suppressed a backup-unit failure" >&2
  exit 1
fi
SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-distinct-rollback \
  previous current
if SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-distinct-rollback \
    current current >/dev/null 2>&1; then
  echo "same-schema rollback accepted the active release" >&2
  exit 1
fi

ln -s RELEASE-METADATA.json "$temporary/payload/unexpected-link"
if SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-payload-tree \
    "$temporary/payload" >/dev/null 2>&1; then
  echo "remote payload policy accepted a symlink" >&2
  exit 1
fi
rm "$temporary/payload/unexpected-link"

production_environment="$temporary/production.env"
printf '%s\n' \
  'SANGUINIUS_ADMIN_COMMANDS_ENABLED=false' \
  'SANGUINIUS_TEST_MODE=false' \
  'SANGUINIUS_VOICE_INPUT_ENABLED=false' \
  'SANGUINIUS_VOICE_INPUT_GUILD_CONSENT_ATTESTED=false' \
  'SANGUINIUS_TRANSCRIPTION_PROVIDER=disabled' \
  'SANGUINIUS_CHRONICLE_ENABLED=true' \
  'SANGUINIUS_TAROT_ENABLED=true' \
  'SANGUINIUS_TAROT_HOUSE_ENABLED=true' \
  'SANGUINIUS_TAROT_INTEGRATION_ENABLED=true' \
  'SANGUINIUS_VOX_ENABLED=true' \
  'SANGUINIUS_VOX_NARRATION_ENABLED=true' \
  'SANGUINIUS_TTS_PROVIDER=openai' \
  'SANGUINIUS_APPEARANCES_MODE=dry_run' \
  'SANGUINIUS_DATABASE_FILE=/var/lib/sanguinius/sanguinius.sqlite3' \
  'SANGUINIUS_LOG_FILE=/var/log/sanguinius/messages.log' \
  'SANGUINIUS_OPERATIONS_STATUS_FILE=/var/lib/sanguinius/runtime/operations-status.json' \
  'SANGUINIUS_BACKUP_DIRECTORY=/var/backups/sanguinius' \
  'SANGUINIUS_TTS_CACHE_DIRECTORY=/var/cache/sanguinius/tts' \
  'SANGUINIUS_TTS_CACHE_MAXIMUM_MIB=64' \
  'SANGUINIUS_TTS_CACHE_MAXIMUM_DAYS=14' \
  'SANGUINIUS_PERSONA_FILE=/opt/sanguinius/current/config/persona.txt' \
  'SANGUINIUS_APPEARANCE_POLICY_FILE=/opt/sanguinius/current/config/appearance-policy-v2.json' \
  'SANGUINIUS_TAROT_DECK_FILE=/opt/sanguinius/current/config/emperor-tarot-v1.json' \
  'SANGUINIUS_TAROT_HOUSE_FILE=/opt/sanguinius/current/config/tarot-house-v1.json' \
  'SANGUINIUS_TTS_FALLBACK_DIRECTORY=/opt/sanguinius/current/assets/vox' \
  'SANGUINIUS_FFMPEG_PATH=/usr/bin/ffmpeg' \
  'SANGUINIUS_FFPROBE_PATH=/usr/bin/ffprobe' >"$production_environment"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-production-environment "$production_environment"
cp "$production_environment" "$temporary/production.env.saved"
sed -i '/SANGUINIUS_TTS_CACHE_MAXIMUM_MIB=/d' "$production_environment"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-production-environment "$production_environment" \
    >/dev/null 2>&1; then
  echo "production environment policy accepted a missing cache ceiling" >&2
  exit 1
fi
cp "$temporary/production.env.saved" "$production_environment"
sed -i 's/SANGUINIUS_TAROT_INTEGRATION_ENABLED=true/SANGUINIUS_TAROT_INTEGRATION_ENABLED=false/' \
  "$production_environment"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-production-environment "$production_environment" \
    >/dev/null 2>&1; then
  echo "production environment policy accepted disabled Tarot integration" >&2
  exit 1
fi
mv "$temporary/production.env.saved" "$production_environment"
printf 'SANGUINIUS_TOKEN=forbidden\n' >>"$production_environment"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-production-environment "$production_environment" \
    >/dev/null 2>&1; then
  echo "production environment policy accepted a direct credential" >&2
  exit 1
fi
sed -i '/SANGUINIUS_TOKEN=forbidden/d' "$production_environment"
printf '  OPENAI_API_KEY = forbidden\n' >>"$production_environment"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-production-environment "$production_environment" \
    >/dev/null 2>&1; then
  echo "production environment policy accepted an indented credential" >&2
  exit 1
fi
sed -i '/OPENAI_API_KEY/d' "$production_environment"
printf '  SANGUINIUS_TEST_MODE=true\n' >>"$production_environment"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-production-environment "$production_environment" \
    >/dev/null 2>&1; then
  echo "production environment policy accepted an indented safety override" >&2
  exit 1
fi
sed -i '/  SANGUINIUS_TEST_MODE=true/d' "$production_environment"
printf 'SANGUINIUS_TEST_MODE=true\n' >>"$production_environment"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-production-environment "$production_environment" \
    >/dev/null 2>&1; then
  echo "production environment policy accepted a duplicate safety setting" >&2
  exit 1
fi

mkdir -p "$temporary/runtime/deploy-stale/nested"
printf stale >"$temporary/runtime/deploy-stale/nested/file"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-remove-managed-tree "$temporary/runtime/deploy-stale"
[[ ! -e $temporary/runtime/deploy-stale ]]
mkdir -p "$temporary/not-managed"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-remove-managed-tree "$temporary/not-managed" \
    >/dev/null 2>&1; then
  echo "managed cleanup accepted an unrelated directory" >&2
  exit 1
fi

printf original >"$temporary/pre.sqlite3"
zstd -q -o "$temporary/pre.sqlite3.zst" "$temporary/pre.sqlite3"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-backup-sidecars "$temporary/pre.sqlite3.zst" \
  "$temporary/pre.sqlite3" 13 \
  0123456789abcdef0123456789abcdef01234567 test-release \
  test-release+rollback-drill pre-migration
[[ -f $temporary/pre.sqlite3.json && -f $temporary/pre.sqlite3.sha256 ]]
grep -Fq '"archive":"pre.sqlite3.zst"' "$temporary/pre.sqlite3.json"
grep -Fq '"deployment_id":"test-release+rollback-drill"' \
  "$temporary/pre.sqlite3.json"
grep -Fq '"original_sha256"' "$temporary/pre.sqlite3.json"
[[ ! -e $temporary/pre.sqlite3.zst.sha256 ]]

grep -Fq 'systemctl enable sanguinius.service' "$remote"
grep -Fq 'new_id=$(install_release "$release_root" true)' "$remote"
grep -Fq '/usr/bin/env' "$remote"
grep -Fq 'run_candidate_config_check "$new"' "$remote"
grep -Fq 'atomic_link "releases/$new_id" "$operations"' "$remote"
grep -Fq '/opt/sanguinius/operations/libexec/sanguinius-backup.bash' \
  "$repository/packaging/systemd/sanguinius-backup.service"
grep -Fxq 'CapabilityBoundingSet=CAP_DAC_OVERRIDE' \
  "$repository/packaging/systemd/sanguinius-backup.service"
grep -Fxq 'AmbientCapabilities=CAP_DAC_OVERRIDE' \
  "$repository/packaging/systemd/sanguinius-backup.service"
bootstrap_backup_line=$(grep -nF \
  'SANGUINIUS_DATABASE_FILE="$database" "$rollback/bin/sanguinius" db backup "$raw"' \
  "$remote" | cut -d: -f1)
bootstrap_ownership_line=$(grep -nF \
  'install -d -m 0750 -o sanguinius -g sanguinius "$state"' \
  "$remote" | cut -d: -f1)
mapfile -t database_ownership_lines < <(grep -nF \
  'adopt_database_service_ownership "$database" sanguinius sanguinius' \
  "$remote" | cut -d: -f1)
[[ ${#database_ownership_lines[@]} -eq 2 ]]
bootstrap_companion_ownership_line=${database_ownership_lines[0]}
deploy_companion_ownership_line=${database_ownership_lines[1]}
mapfile -t operation_lock_lines < <(grep -nF \
  'open_operations_lock "$runtime" "$lock_file"' "$remote" | cut -d: -f1)
[[ ${#operation_lock_lines[@]} -eq 3 ]]
deploy_lock_line=${operation_lock_lines[1]}
deploy_process_line=$(grep -nF 'assert_process_state "$active_state"' \
  "$remote" | tail -n 1 | cut -d: -f1)
[[ $bootstrap_backup_line =~ ^[0-9]+$ &&
   $bootstrap_ownership_line =~ ^[0-9]+$ &&
   $bootstrap_companion_ownership_line =~ ^[0-9]+$ &&
   $deploy_companion_ownership_line =~ ^[0-9]+$ &&
   $deploy_lock_line =~ ^[0-9]+$ &&
   $deploy_process_line =~ ^[0-9]+$ &&
   $bootstrap_backup_line -lt $bootstrap_ownership_line &&
   $bootstrap_ownership_line -lt $bootstrap_companion_ownership_line &&
   $deploy_lock_line -lt $deploy_companion_ownership_line &&
   $deploy_process_line -lt $deploy_companion_ownership_line ]]

counts_database="$temporary/counts.sqlite3"
sqlite3 "$counts_database" \
  'CREATE TABLE discord_user(value); CREATE TABLE chronicle_entry(value); CREATE TABLE relationship_event(value); CREATE TABLE tarot_transaction(value); CREATE TABLE tarot_posting(value); CREATE TABLE tarot_wager(value); CREATE TABLE voice_session(value); CREATE TABLE speech_item(value); INSERT INTO discord_user VALUES(1); INSERT INTO tarot_transaction VALUES(1); INSERT INTO tarot_transaction VALUES(2); INSERT INTO speech_item VALUES(1);'
counts=$(SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-safe-counts "$counts_database")
[[ $counts == 1:0:0:2:0:0:0:1 ]]
printf untouched >"$temporary/database-companion-target"
ln -s "$temporary/database-companion-target" "$counts_database.lock"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-database-filesystem "$counts_database" \
    >/dev/null 2>&1; then
  echo "deployment accepted a symbolic database lock" >&2
  exit 1
fi
[[ $(<"$temporary/database-companion-target") == untouched ]]
unlink "$counts_database.lock"
ln -s "$temporary/missing-database-wal" "$counts_database-wal"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-database-filesystem "$counts_database" \
    >/dev/null 2>&1; then
  echo "deployment accepted a dangling database sidecar" >&2
  exit 1
fi
unlink "$counts_database-wal"
mkfifo "$counts_database-shm"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-database-filesystem "$counts_database" \
    >/dev/null 2>&1; then
  echo "deployment accepted a nonregular database sidecar" >&2
  exit 1
fi
unlink "$counts_database-shm"
ln "$temporary/database-companion-target" "$counts_database-journal"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-database-filesystem "$counts_database" \
    >/dev/null 2>&1; then
  echo "deployment accepted a hard-linked database sidecar" >&2
  exit 1
fi
[[ $(<"$temporary/database-companion-target") == untouched ]]
unlink "$counts_database-journal"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-database-filesystem "$counts_database"

ownership_database="$temporary/ownership.sqlite3"
install -m 0644 /dev/null "$ownership_database"
for suffix in .lock -wal -shm -journal; do
  install -m 0644 /dev/null "$ownership_database$suffix"
done
ownership_user=$(id -un)
ownership_group=$(id -gn)
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-database-ownership "$ownership_database" \
  "$ownership_user" "$ownership_group"
ownership_identity="$(id -u):$(id -g):600"
for suffix in '' .lock -wal -shm -journal; do
  [[ $(stat -c '%u:%g:%a' "$ownership_database$suffix") == \
     "$ownership_identity" ]]
done

atomic_binary="$temporary/atomic-release/bin/sanguinius"
mkdir -p "$(dirname "$atomic_binary")" "$temporary/atomic-runtime"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'set -euo pipefail' \
  'database=${SANGUINIUS_DATABASE_FILE:?}' \
  'case "${1:-} ${2:-} ${3:-}" in' \
  '  "db migrate ") sqlite3 "$database" "PRAGMA journal_mode=WAL; PRAGMA user_version=13;" >/dev/null ;;' \
  '  "db check ") [[ $(sqlite3 "$database" "PRAGMA user_version;") == 13 ]] ;;' \
  '  "db integrity ") [[ $(sqlite3 "$database" "PRAGMA integrity_check;") == ok ]] ;;' \
  '  "db relationships check"|"db tarot check") : ;;' \
  '  "db backup "*) sqlite3 "$database" ".backup $3" ;;' \
  '  "db status ") printf "database=current\ncurrent_schema=%s\ntarget_schema=13\npending_migrations=0\nsqlite=test\n" "$(sqlite3 "$database" "PRAGMA user_version;")" ;;' \
  '  *) exit 2 ;;' \
  'esac' >"$atomic_binary"
chmod 0755 "$atomic_binary"
atomic_database="$temporary/atomic.sqlite3"
atomic_source="$temporary/atomic-source.sqlite3"
sqlite3 "$atomic_database" \
  "PRAGMA user_version=16; CREATE TABLE marker(value); INSERT INTO marker VALUES('original');"
sqlite3 "$atomic_source" \
  "PRAGMA user_version=13; CREATE TABLE marker(value); INSERT INTO marker VALUES('restored');"
zstd -q -o "$temporary/atomic-source.sqlite3.zst" "$atomic_source"
printf corrupt >"$temporary/atomic-corrupt.zst"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-atomic-database-restore "$temporary/atomic-corrupt.zst" \
    "$atomic_binary" 13 "$atomic_database" "$temporary/atomic-runtime" \
    corrupt >/dev/null 2>&1; then
  echo "atomic restore accepted a corrupt archive" >&2
  exit 1
fi
[[ $(sqlite3 "$atomic_database" 'SELECT value FROM marker;') == original ]]
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    SANGUINIUS_TEST_FAIL_ATOMIC_RESTORE_SWITCH=true \
    "$remote" policy-atomic-database-restore \
    "$temporary/atomic-source.sqlite3.zst" "$atomic_binary" 13 \
    "$atomic_database" "$temporary/atomic-runtime" interrupted \
    >/dev/null 2>&1; then
  echo "injected atomic restore switch failure unexpectedly succeeded" >&2
  exit 1
fi
[[ $(sqlite3 "$atomic_database" 'SELECT value FROM marker;') == original ]]
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-atomic-database-restore \
  "$temporary/atomic-source.sqlite3.zst" "$atomic_binary" 13 \
  "$atomic_database" "$temporary/atomic-runtime" applied >/dev/null
[[ $(sqlite3 "$atomic_database" 'SELECT value FROM marker;') == restored ]]
find "$temporary/atomic-runtime" -maxdepth 2 -type f \
  -path '*/failed-applied.*/sanguinius.sqlite3' -print -quit | grep -q .

SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-capacity 1048576 1024
if SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-capacity 1048575 1024 \
    >/dev/null 2>&1; then
  echo "remote capacity policy accepted insufficient disk" >&2
  exit 1
fi

retention=$(SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-retention \
  current previous current previous keep-b keep-a delete-me ../unrelated)
[[ $retention == delete-me ]]
retention_releases="$temporary/retention-releases"
for candidate in current previous keep-b keep-a delete-me; do
  mkdir -p "$retention_releases/$candidate"
  printf '{"release_id":"%s","deployment_id":"%s"}\n' \
    "$candidate" "$candidate" \
    >"$retention_releases/$candidate/RELEASE-METADATA.json"
done
mkdir -p "$retention_releases/zz-unrecognized" \
  "$retention_releases/yy-conflicting"
printf '{"release_id":"other","deployment_id":"other"}\n' \
  >"$retention_releases/yy-conflicting/RELEASE-METADATA.json"
retention=$(SANGUINIUS_SCRIPT_TESTING=true \
  SANGUINIUS_TEST_ROOT="$temporary" "$remote" policy-release-retention \
  "$retention_releases" current previous)
[[ $retention == delete-me ]]

prune_releases="$temporary/prune-releases"
for candidate in zz-current zy-previous zx-keep zw-keep aa-old-previous; do
  mkdir -p "$prune_releases/$candidate"
  printf '{"release_id":"%s","deployment_id":"%s"}\n' \
    "$candidate" "$candidate" \
    >"$prune_releases/$candidate/RELEASE-METADATA.json"
done
mkdir -p "$prune_releases/unrecognized"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-prune-release-retention "$prune_releases" \
  zz-current zy-previous aa-old-previous
[[ -d $prune_releases/aa-old-previous ]]
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-prune-release-retention "$prune_releases" \
  zz-current zy-previous
[[ ! -e $prune_releases/aa-old-previous &&
   -d $prune_releases/zz-current && -d $prune_releases/zy-previous &&
   -d $prune_releases/zx-keep && -d $prune_releases/zw-keep &&
   -d $prune_releases/unrecognized ]]
[[ $(SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-failure 16 16 true) == \
   rollback-release ]]
[[ $(SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-failure 13 16 false) == \
   restore-database ]]
[[ $(SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-failure 13 16 true) == \
   preserve-schema ]]
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'set -euo pipefail' \
  'printf "%s\n" "$*" >>"$SANGUINIUS_FAKE_SYSTEMCTL_LOG"' \
  'case ${1:-} in' \
  '  start) printf "active\n" >"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE" ;;' \
  '  stop)' \
  '    [[ ${SANGUINIUS_FAKE_SYSTEMCTL_FAIL_STOP:-false} != true ]] || exit 1' \
  '    printf "inactive\n" >"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE" ;;' \
  '  disable)' \
  '    [[ ${SANGUINIUS_FAKE_SYSTEMCTL_FAIL_DISABLE:-false} != true ]] || exit 1' \
  '    printf "disabled\n" >"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED" ;;' \
  '  enable) printf "enabled\n" >"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED" ;;' \
  '  is-active) cat "$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE" ;;' \
  '  is-enabled) cat "$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED" ;;' \
  '  show)' \
  '    case "$*" in' \
  '      "show -p ActiveState --value sanguinius.service") cat "$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE" ;;' \
  '      "show -p StatusText --value sanguinius.service") cat "$SANGUINIUS_FAKE_SYSTEMCTL_STATUS" ;;' \
  '      "show -p MainPID --value sanguinius.service") cat "$SANGUINIUS_FAKE_SYSTEMCTL_MAIN_PID" ;;' \
  '      "show -p NRestarts --value sanguinius.service") cat "$SANGUINIUS_FAKE_SYSTEMCTL_RESTARTS" ;;' \
  '      *) exit 1 ;;' \
  '    esac ;;' \
  'esac' \
  >"$temporary/bin/systemctl"
chmod 0755 "$temporary/bin/systemctl"
export SANGUINIUS_FAKE_SYSTEMCTL_LOG="$temporary/systemctl.log"
export SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE="$temporary/systemctl-active"
export SANGUINIUS_FAKE_SYSTEMCTL_ENABLED="$temporary/systemctl-enabled"
export SANGUINIUS_FAKE_SYSTEMCTL_STATUS="$temporary/systemctl-status"
export SANGUINIUS_FAKE_SYSTEMCTL_MAIN_PID="$temporary/systemctl-main-pid"
export SANGUINIUS_FAKE_SYSTEMCTL_RESTARTS="$temporary/systemctl-restarts"
printf 'inactive\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE"
printf 'disabled\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED"
printf 'Waiting for Discord readiness\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_STATUS"
printf '0\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_MAIN_PID"
printf '0\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_RESTARTS"

export SANGUINIUS_FAKE_PGREP_PROCESSES="$temporary/pgrep-processes"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'set -euo pipefail' \
  '[[ $# -eq 2 && $1 == -x && $2 == sanguinius ]] || exit 1' \
  'cat "$SANGUINIUS_FAKE_PGREP_PROCESSES"' \
  >"$temporary/bin/pgrep"
chmod 0755 "$temporary/bin/pgrep"
printf 'active\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE"
printf 'Ready; Discord connected and commands synchronized\n' \
  >"$SANGUINIUS_FAKE_SYSTEMCTL_STATUS"
printf '4242\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_MAIN_PID"
printf '3\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_RESTARTS"
printf '4242\n' >"$SANGUINIUS_FAKE_PGREP_PROCESSES"
PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true "$remote" \
  policy-candidate-health 3
printf '4\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_RESTARTS"
if PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true "$remote" \
    policy-candidate-health 3 >/dev/null 2>&1; then
  echo "candidate health accepted an intervening restart" >&2
  exit 1
fi
printf '3\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_RESTARTS"
printf '4243\n' >"$SANGUINIUS_FAKE_PGREP_PROCESSES"
if PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true "$remote" \
    policy-candidate-health 3 >/dev/null 2>&1; then
  echo "candidate health accepted an unexpected process" >&2
  exit 1
fi
rm -f "$temporary/bin/pgrep"
rm -f "$SANGUINIUS_FAKE_SYSTEMCTL_LOG"

printf 'inactive\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE"
printf 'disabled\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED"
PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true "$remote" \
  policy-restore-active-state inactive
[[ ! -e $SANGUINIUS_FAKE_SYSTEMCTL_LOG ]]
PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true "$remote" \
  policy-restore-active-state active
grep -Fxq 'start sanguinius.service' "$SANGUINIUS_FAKE_SYSTEMCTL_LOG"
: >"$SANGUINIUS_FAKE_SYSTEMCTL_LOG"
PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true "$remote" \
  policy-ready-failure false false
PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true "$remote" \
  policy-ready-failure true true
[[ ! -s $SANGUINIUS_FAKE_SYSTEMCTL_LOG ]]
PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true "$remote" \
  policy-ready-failure true false
grep -Fxq 'stop sanguinius.service' "$SANGUINIUS_FAKE_SYSTEMCTL_LOG"
grep -Fxq 'disable sanguinius.service' "$SANGUINIUS_FAKE_SYSTEMCTL_LOG"
grep -Fxq 'is-active sanguinius.service' "$SANGUINIUS_FAKE_SYSTEMCTL_LOG"
grep -Fxq 'is-enabled sanguinius.service' "$SANGUINIUS_FAKE_SYSTEMCTL_LOG"
[[ $(<"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE") == inactive &&
   $(<"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED") == disabled ]]

printf 'active\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE"
printf 'enabled\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED"
if PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true \
    SANGUINIUS_FAKE_SYSTEMCTL_FAIL_STOP=true "$remote" \
    policy-ready-failure true false >/dev/null 2>&1; then
  echo "schema-changing fence accepted a failed service stop" >&2
  exit 1
fi
[[ $(<"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE") == active &&
   $(<"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED") == disabled ]]

printf 'active\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE"
printf 'enabled\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED"
if PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true \
    SANGUINIUS_FAKE_SYSTEMCTL_FAIL_DISABLE=true "$remote" \
    policy-ready-failure true false >/dev/null 2>&1; then
  echo "schema-changing fence accepted a failed service disable" >&2
  exit 1
fi
[[ $(<"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE") == inactive &&
   $(<"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED") == enabled ]]

recovery_root="$temporary/deployment-recovery-root"
recovery_releases="$recovery_root/opt/sanguinius/releases"
recovery_state="$recovery_root/var/lib/sanguinius"
recovery_runtime="$recovery_state/runtime"
recovery_database="$recovery_state/sanguinius.sqlite3"
mkdir -p "$recovery_releases/old/bin" "$recovery_releases/old/systemd" \
  "$recovery_releases/prior" "$recovery_releases/operations-old" \
  "$recovery_releases/candidate" "$recovery_runtime" \
  "$recovery_root/etc/systemd/system" "$recovery_root/etc/sysusers.d" \
  "$recovery_root/etc/tmpfiles.d"
cp "$atomic_binary" "$recovery_releases/old/bin/sanguinius"
chmod 0755 "$recovery_releases/old/bin/sanguinius"
printf '13\n' >"$recovery_releases/old/target-schema"
printf '{"service_unit":"sanguinius.service"}\n' \
  >"$recovery_releases/old/RELEASE-METADATA.json"
for unit_name in sanguinius.service sanguinius-backup.service \
    sanguinius-backup.timer; do
  printf 'old=%s\n' "$unit_name" \
    >"$recovery_releases/old/systemd/$unit_name"
  printf 'candidate=%s\n' "$unit_name" \
    >"$recovery_root/etc/systemd/system/$unit_name"
done
printf 'u sanguinius - original\n' \
  >"$recovery_root/etc/sysusers.d/sanguinius.conf"
printf 'd /var/lib/sanguinius 0750 sanguinius sanguinius -\n' \
  >"$recovery_root/etc/tmpfiles.d/sanguinius.conf"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$recovery_root" \
  "$remote" policy-system-configuration-snapshot \
  "$recovery_root/system-configuration-before"

reset_recovery_candidate() {
  ln -sfn releases/candidate "$recovery_root/opt/sanguinius/current"
  ln -sfn releases/candidate "$recovery_root/opt/sanguinius/previous"
  ln -sfn releases/candidate "$recovery_root/opt/sanguinius/operations"
  printf 'u sanguinius - candidate\n' \
    >"$recovery_root/etc/sysusers.d/sanguinius.conf"
  printf 'd /var/lib/sanguinius 0700 root root -\n' \
    >"$recovery_root/etc/tmpfiles.d/sanguinius.conf"
  for unit_name in sanguinius.service sanguinius-backup.service \
      sanguinius-backup.timer; do
    printf 'candidate=%s\n' "$unit_name" \
      >"$recovery_root/etc/systemd/system/$unit_name"
  done
}

reset_recovery_candidate
sqlite3 "$recovery_database" \
  "PRAGMA user_version=16; CREATE TABLE marker(value); INSERT INTO marker VALUES('candidate');"
printf 'inactive\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE"
printf 'disabled\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED"
PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true \
  SANGUINIUS_TEST_ROOT="$temporary" "$remote" \
  policy-deployment-durable-recovery "$recovery_root" true \
  "$temporary/atomic-source.sqlite3.zst" 13
[[ $(sqlite3 "$recovery_database" 'SELECT value FROM marker;') == restored ]]
[[ $(readlink "$recovery_root/opt/sanguinius/current") == releases/old &&
   $(readlink "$recovery_root/opt/sanguinius/previous") == releases/prior &&
   $(readlink "$recovery_root/opt/sanguinius/operations") == \
     releases/operations-old ]]
for unit_name in sanguinius.service sanguinius-backup.service \
    sanguinius-backup.timer; do
  cmp "$recovery_releases/old/systemd/$unit_name" \
    "$recovery_root/etc/systemd/system/$unit_name"
done
grep -Fq 'original' "$recovery_root/etc/sysusers.d/sanguinius.conf"
grep -Fq '0750 sanguinius sanguinius' \
  "$recovery_root/etc/tmpfiles.d/sanguinius.conf"

rm -f "$recovery_database" "$recovery_database-wal" \
  "$recovery_database-shm" "$recovery_database-journal"
sqlite3 "$recovery_database" \
  "PRAGMA user_version=16; CREATE TABLE marker(value); INSERT INTO marker VALUES('same-schema-candidate');"
reset_recovery_candidate
PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true \
  SANGUINIUS_TEST_ROOT="$temporary" "$remote" \
  policy-deployment-durable-recovery "$recovery_root" false - 16
[[ $(sqlite3 "$recovery_database" 'SELECT value FROM marker;') == \
  same-schema-candidate ]]
[[ $(readlink "$recovery_root/opt/sanguinius/current") == releases/old ]]
grep -Fq 'original' "$recovery_root/etc/sysusers.d/sanguinius.conf"

rm -f "$recovery_database" "$recovery_database-wal" \
  "$recovery_database-shm" "$recovery_database-journal"
sqlite3 "$recovery_database" \
  "PRAGMA user_version=16; CREATE TABLE marker(value); INSERT INTO marker VALUES('failed-recovery-original');"
reset_recovery_candidate
printf 'inactive\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE"
printf 'enabled\n' >"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED"
: >"$SANGUINIUS_FAKE_SYSTEMCTL_LOG"
if PATH="$temporary/bin:$PATH" SANGUINIUS_SCRIPT_TESTING=true \
    SANGUINIUS_TEST_ROOT="$temporary" \
    SANGUINIUS_TEST_FAIL_ATOMIC_RESTORE_SWITCH=true "$remote" \
    policy-deployment-durable-recovery "$recovery_root" true \
    "$temporary/atomic-source.sqlite3.zst" 13 >/dev/null 2>&1; then
  echo "deployment accepted an incomplete database recovery" >&2
  exit 1
fi
[[ $(sqlite3 "$recovery_database" 'SELECT value FROM marker;') == \
  failed-recovery-original ]]
[[ $(readlink "$recovery_root/opt/sanguinius/current") == releases/old &&
   $(<"$SANGUINIUS_FAKE_SYSTEMCTL_ACTIVE") == inactive &&
   $(<"$SANGUINIUS_FAKE_SYSTEMCTL_ENABLED") == disabled ]]
grep -Fxq 'disable sanguinius.service' "$SANGUINIUS_FAKE_SYSTEMCTL_LOG"
grep -Fxq 'stop sanguinius-backup.timer' "$SANGUINIUS_FAKE_SYSTEMCTL_LOG"

grep -Fq 'if ! stop_candidate_for_recovery; then' "$remote"
grep -Fq 'fence_schema_changing_candidate ||' "$remote"
if grep -Eq 'systemctl (stop|disable) sanguinius\.service.*\|\| true' "$remote"; then
  echo "deployment still suppresses a candidate stop/disable failure" >&2
  exit 1
fi

stop_marker_line=$(grep -nF '    service_stopped_for_deploy=true' "$remote" |
  cut -d: -f1)
authoritative_backup_line=$(grep -nF \
  '  authoritative_raw="$disposable/pre-migration-authoritative.sqlite3"' \
  "$remote" | cut -d: -f1)
production_migration_line=$(grep -nF \
  '  SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db migrate || {' \
  "$remote" | cut -d: -f1)
[[ $stop_marker_line =~ ^[0-9]+$ &&
   $authoritative_backup_line =~ ^[0-9]+$ &&
   $production_migration_line =~ ^[0-9]+$ &&
   $stop_marker_line -lt $authoritative_backup_line &&
   $authoritative_backup_line -lt $production_migration_line ]]

rollback_section=$(sed -n '/^rollback-same-schema)/,/^\*)/p' "$remote")
rollback_stop_line=$(grep -nF 'systemctl stop sanguinius.service' \
  <<<"$rollback_section" | cut -d: -f1)
rollback_process_line=$(grep -nF 'assert_process_state inactive' \
  <<<"$rollback_section" | cut -d: -f1)
rollback_exclusive_line=$(grep -nF 'assert_database_exclusive' \
  <<<"$rollback_section" | cut -d: -f1)
rollback_distinct_line=$(grep -nF \
  'require_distinct_rollback_target "$release_id" "$old_id"' \
  <<<"$rollback_section" | cut -d: -f1)
rollback_switch_line=$(grep -nF \
  'atomic_link "releases/$release_id" "$current"' \
  <<<"$rollback_section" | cut -d: -f1)
[[ $rollback_stop_line =~ ^[0-9]+$ &&
   $rollback_process_line =~ ^[0-9]+$ &&
   $rollback_exclusive_line =~ ^[0-9]+$ &&
   $rollback_distinct_line =~ ^[0-9]+$ &&
   $rollback_switch_line =~ ^[0-9]+$ &&
   $rollback_distinct_line -lt $rollback_stop_line &&
   $rollback_stop_line -lt $rollback_process_line &&
   $rollback_process_line -lt $rollback_exclusive_line &&
   $rollback_exclusive_line -lt $rollback_switch_line ]]

deploy_section=$(sed -n '/^deploy)/,/^rollback-same-schema)/p' "$remote")
[[ $(grep -Fc 'install_release_units "$new"' <<<"$deploy_section") -eq 1 ]]
[[ $(grep -Fc 'recover_predeployment_durable_state false' \
  <<<"$deploy_section") -ge 2 ]]
[[ $(grep -Fc 'recover_predeployment_durable_state true' \
  <<<"$deploy_section") -ge 2 ]]
snapshot_line=$(grep -nF \
  'snapshot_system_configuration_files "$system_configuration_snapshot"' \
  <<<"$deploy_section" | cut -d: -f1)
setup_marker_line=$(grep -nF 'candidate_durable_setup_started=true' \
  <<<"$deploy_section" | cut -d: -f1)
current_switch_line=$(grep -nF 'atomic_link "releases/$new_id" "$current"' \
  <<<"$deploy_section" | head -n1 | cut -d: -f1)
[[ $snapshot_line =~ ^[0-9]+$ && $setup_marker_line =~ ^[0-9]+$ &&
   $current_switch_line =~ ^[0-9]+$ &&
   $snapshot_line -lt $setup_marker_line &&
   $setup_marker_line -lt $current_switch_line ]]
for setup_step in current-link release-units system-configuration daemon-reload \
    operations-link; do
  grep -Fq "inject_deploy_setup_failure $setup_step" <<<"$deploy_section"
done
operations_link_line=$(grep -nF \
  'atomic_link "releases/$new_id" "$operations"' <<<"$deploy_section" | \
  cut -d: -f1)
candidate_start_line=$(grep -nF 'systemctl start sanguinius.service || {' \
  <<<"$deploy_section" | cut -d: -f1)
operations_backup_line=$(grep -nF \
  '"$operations/libexec/sanguinius-backup.bash" manual --lock-held' \
  <<<"$deploy_section" | cut -d: -f1)
mapfile -t candidate_health_lines < <(grep -nF \
  'candidate_service_health_verified "$candidate_restart_baseline"' \
  <<<"$deploy_section" | cut -d: -f1)
success_status_line=$(grep -nF 'write_status succeeded "$target_schema"' \
  <<<"$deploy_section" | cut -d: -f1)
[[ $operations_link_line =~ ^[0-9]+$ &&
   $candidate_start_line =~ ^[0-9]+$ &&
   $operations_backup_line =~ ^[0-9]+$ &&
   ${#candidate_health_lines[@]} -eq 2 &&
   $success_status_line =~ ^[0-9]+$ &&
   $operations_link_line -lt $candidate_start_line &&
   $candidate_start_line -lt $operations_backup_line &&
   $operations_backup_line -lt ${candidate_health_lines[1]} &&
   ${candidate_health_lines[1]} -lt $success_status_line ]]
post_migration_failure_section=$(sed -n \
  '/if ! SANGUINIUS_DATABASE_FILE="\$database" "\$new\/bin\/sanguinius" db check ||/,/candidate_durable_setup_started=true/p' \
  <<<"$deploy_section")
grep -Fq 'recover_predeployment_durable_state true' \
  <<<"$post_migration_failure_section"
grep -Fq 'candidate_failure_handled=true' \
  <<<"$post_migration_failure_section"
if grep -Fq 'atomic_restore_database "$pre"' \
    <<<"$post_migration_failure_section"; then
  echo "post-migration verification bypasses fenced durable recovery" >&2
  exit 1
fi

mkdir "$temporary/releases"
ln -s releases/old "$temporary/current"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-atomic-link releases/new "$temporary/current"
[[ $(readlink "$temporary/current") == releases/new ]]
[[ ! -e $temporary/unrelated ]]

exec 8>"$temporary/operations.lock"
flock -n 8
if flock -n "$temporary/operations.lock" true; then
  echo "deployment lock contention was not enforced" >&2
  exit 1
fi
flock -u 8

echo "deployment shell tests passed"
