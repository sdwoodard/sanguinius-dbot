#!/usr/bin/env bash
# shellcheck disable=SC2016
set -euo pipefail
umask 0077

repository=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
temporary=$(mktemp -d /tmp/sanguinius-backup-test.XXXXXXXX)
cleanup() {
  find -P "$temporary" -type f -delete
  find -P "$temporary" -type l -delete
  find -P "$temporary" -depth -type d -empty -delete
}
trap cleanup EXIT

for script in "$repository/scripts/sanguinius-backup.bash" \
              "$repository/scripts/sanguinius-restore.bash"; do
  bash -n "$script"
done

revision=0123456789abcdef0123456789abcdef01234567
release16=release-schema16
release13=release-schema13
state="$temporary/var/lib/sanguinius"
runtime="$state/runtime"
backups="$temporary/var/backups/sanguinius"
releases="$temporary/opt/sanguinius/releases"
current="$temporary/opt/sanguinius/current"
previous="$temporary/opt/sanguinius/previous"
database="$state/sanguinius.sqlite3"
unit="$temporary/etc/systemd/system/sanguinius.service"
mkdir -p "$runtime" "$backups" "$releases/$release16/bin" \
  "$releases/$release16/systemd" "$releases/$release13/bin" \
  "$releases/$release13/systemd" "$(dirname "$unit")"

fake="$temporary/fake-sanguinius"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'set -euo pipefail' \
  'release=$(cd "$(dirname "$0")/.." && pwd -P)' \
  'target=$(<"$release/target-schema")' \
  'database=${SANGUINIUS_DATABASE_FILE:-}' \
  'printf "%s %s\n" "${2:-version}" "$database" >>"$release/invocations.log"' \
  'if [[ ${1:-} == --version ]]; then printf "{\"version\":\"2.2.0\",\"revision\":\"0123456789abcdef0123456789abcdef01234567\",\"release_id\":\"%s\",\"schema_target\":%s,\"command_catalog_version\":16}\n" "$(basename "$release")" "$target"; exit; fi' \
  '[[ ${1:-} == db && -n $database ]] || exit 2' \
  'current=$(sqlite3 "$database" "PRAGMA user_version;")' \
  'case ${2:-} in' \
  '  status) printf "database=current\ncurrent_schema=%s\ntarget_schema=%s\npending_migrations=%s\nsqlite=test\n" "$current" "$target" "$((target-current))" ;;' \
  '  backup) if [[ -f $release/fail-backup ]]; then printf partial >"$3"; exit 1; fi; sqlite3 "$database" ".backup $3" ;;' \
  '  migrate) [[ ! -f $release/fail-migrate ]] || exit 1; sqlite3 "$database" "PRAGMA user_version=$target;" ;;' \
  '  check) [[ $current == "$target" ]]; [[ ! -f $release/fail-production-check || $database != */var/lib/sanguinius/sanguinius.sqlite3 ]] ;;' \
  '  integrity) [[ $(sqlite3 "$database" "PRAGMA integrity_check;") == ok ]]; echo integrity=ok ;;' \
  '  relationships) (( target < 16 )); [[ ${3:-} == check ]]; echo relationships=ok ;;' \
  '  tarot) (( target < 16 )); [[ ${3:-} == check ]]; echo tarot=ok ;;' \
  '  invariants) (( target >= 16 )); [[ ${3:-} == check ]]; echo invariants=ok ;;' \
  '  *) exit 2 ;;' \
  'esac' >"$fake"
chmod 0755 "$fake"

make_release() {
  local id=$1 schema=$2 service=$3
  local compatibility=false
  [[ $service == sanguinius.service ]] || compatibility=true
  cp "$fake" "$releases/$id/bin/sanguinius"
  chmod 0755 "$releases/$id/bin/sanguinius"
  printf '%s\n' "$schema" >"$releases/$id/target-schema"
  printf '[Service]\nType=%s\n' \
    "$([[ $service == sanguinius.service ]] && printf notify || printf simple)" \
    >"$releases/$id/systemd/$service"
  printf '{"version":"2.2.0","revision":"%s","release_id":"%s","schema_target":%s,"command_catalog_version":16,"service_unit":"%s","compatibility_release":%s}\n' \
    "$revision" "$id" "$schema" "$service" "$compatibility" \
    >"$releases/$id/RELEASE-METADATA.json"
}
make_release "$release16" 16 sanguinius.service
make_release "$release13" 13 sanguinius-compat.service
[[ ! -d $releases/$release13/libexec ]]

set_active() {
  local active=$1 prior=$2 schema=$3 marker=$4
  ln -sfn "releases/$active" "$current"
  ln -sfn "releases/$prior" "$previous"
  install -m 0644 "$releases/$active/systemd/$(sed -n 's/.*"service_unit":"\([A-Za-z0-9.-]*\)".*/\1/p' "$releases/$active/RELEASE-METADATA.json")" "$unit"
  printf '%s\n' "$schema" >"$state/state-version"
  rm -f "$database" "$database-wal" "$database-shm" "$database-journal"
  sqlite3 "$database" "PRAGMA journal_mode=WAL; PRAGMA user_version=$schema; CREATE TABLE marker(value TEXT NOT NULL); INSERT INTO marker VALUES('$marker'); PRAGMA wal_checkpoint(TRUNCATE);" >/dev/null
}
set_active "$release16" "$release13" 16 schema16-fixture

exec 8>"$runtime/operations.lock"
flock -n 8
set +e
SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  "$repository/scripts/sanguinius-backup.bash" manual >/dev/null 2>&1
locked_status=$?
set -e
flock -u 8
[[ $locked_status -eq 75 ]]

SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  "$repository/scripts/sanguinius-backup.bash" rolling >/dev/null
archive16=$(find "$backups" -type f -name '*-schema16-*-rolling.sqlite3.zst' -print -quit)
[[ -n $archive16 && -f ${archive16%.zst}.json && -f ${archive16%.zst}.sha256 ]]
SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  "$repository/scripts/sanguinius-restore.bash" verify "$archive16" \
  --release "$releases/$release16" >/dev/null

sqlite3 "$database" "UPDATE marker SET value='changed-before-restore';"
find "$backups" -maxdepth 1 -type f -name '*-manual.sqlite3.*' -delete
SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  "$repository/scripts/sanguinius-restore.bash" apply "$archive16" \
  --release "$releases/$release16" --confirm >/dev/null
[[ $(sqlite3 "$database" 'SELECT value FROM marker;') == schema16-fixture ]]

find "$backups" -maxdepth 1 -type f -name '*-manual.sqlite3.*' -delete
sqlite3 "$database" "UPDATE marker SET value='retained-after-failure';"
touch "$releases/$release16/fail-production-check"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" apply "$archive16" \
    --release "$releases/$release16" --confirm >/dev/null 2>&1; then
  echo "restore apply ignored a post-install verification failure" >&2
  exit 1
fi
[[ $(sqlite3 "$database" 'SELECT value FROM marker;') == retained-after-failure ]]
rm "$releases/$release16/fail-production-check"

if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" apply "$archive16" \
    --release "$releases/$release16" >/dev/null 2>&1; then
  echo "restore apply did not require its confirmation guard" >&2
  exit 1
fi
touch "$releases/$release16/fail-migrate"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" verify "$archive16" \
    --release "$releases/$release16" >/dev/null 2>&1; then
  echo "restore verification ignored a migration failure" >&2
  exit 1
fi
rm "$releases/$release16/fail-migrate"

cp "$releases/$release16/RELEASE-METADATA.json" "$temporary/release-metadata.saved"
sed -i 's/"schema_target":16/"schema_target":15/' \
  "$releases/$release16/RELEASE-METADATA.json"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" verify "$archive16" \
    --release "$releases/$release16" >/dev/null 2>&1; then
  echo "restore verification accepted an incompatible release" >&2
  exit 1
fi
mv "$temporary/release-metadata.saved" "$releases/$release16/RELEASE-METADATA.json"

ln -s "$archive16" "$temporary/archive-link.zst"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" verify \
    "$temporary/archive-link.zst" --release "$releases/$release16" \
    >/dev/null 2>&1; then
  echo "restore verification followed an archive symlink" >&2
  exit 1
fi

mv "$runtime/operations.lock" "$runtime/operations.lock.saved"
printf untouched >"$temporary/lock-target"
ln -s "$temporary/lock-target" "$runtime/operations.lock"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-backup.bash" failure >/dev/null 2>&1; then
  echo "backup followed an operations-lock symlink" >&2
  exit 1
fi
[[ $(<"$temporary/lock-target") == untouched ]]
unlink "$runtime/operations.lock"
mv "$runtime/operations.lock.saved" "$runtime/operations.lock"

find "$backups" -maxdepth 1 -type f -name '*-rolling.sqlite3.*' -delete
for index in 01 02 03 04 05 06 07 08; do
  base="202001${index}T000000Z-schema16-0123456789ab-rolling.sqlite3"
  printf old >"$backups/$base.zst"
  printf old >"$backups/$base.json"
  printf old >"$backups/$base.sha256"
done
printf preserve >"$backups/legacy-backup.zst"
printf preserve >"$backups/unrecognized-rolling.sqlite3.zst"
printf preserve >"$backups/keep-manual.sqlite3.json"
SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  "$repository/scripts/sanguinius-backup.bash" rolling >/dev/null
[[ $(find "$backups" -maxdepth 1 -type f -name '*-rolling.sqlite3.json' | wc -l) -eq 7 ]]
[[ -f $backups/legacy-backup.zst &&
   -f $backups/unrecognized-rolling.sqlite3.zst &&
   -f $backups/keep-manual.sqlite3.json ]]

artifact_count=$(find "$backups" -maxdepth 1 -type f | wc -l)
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    SANGUINIUS_TEST_FAIL_BACKUP_PUBLICATION=true \
    "$repository/scripts/sanguinius-backup.bash" failure >/dev/null 2>&1; then
  echo "injected backup publication failure unexpectedly succeeded" >&2
  exit 1
fi
[[ $(find "$backups" -maxdepth 1 -type f | wc -l) -eq $artifact_count ]]
touch "$releases/$release16/fail-backup"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-backup.bash" failure >/dev/null 2>&1; then
  echo "interrupted backup unexpectedly succeeded" >&2
  exit 1
fi
rm "$releases/$release16/fail-backup"
[[ $(find "$backups" -maxdepth 1 -type f | wc -l) -eq $artifact_count ]]
grep -q '"backup_result":"failed"' "$runtime/operations-status.json"

set_active "$release13" "$release16" 13 schema13-fixture
SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  "$repository/scripts/sanguinius-backup.bash" pre-migration >/dev/null
archive13=$(find "$backups" -type f -name '*-schema13-*-pre-migration.sqlite3.zst' -print -quit)
[[ -n $archive13 ]]
if grep -q '^invariants ' "$releases/$release13/invocations.log"; then
  echo "schema-13 backup invoked schema-16 umbrella invariants" >&2
  exit 1
fi
set_active "$release16" "$release13" 16 production-schema16
find "$backups" -maxdepth 1 -type f -name '*-manual.sqlite3.*' -delete

SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  SANGUINIUS_TEST_FAIL_RESTORE_QUARANTINE=true \
  "$repository/scripts/sanguinius-restore.bash" apply "$archive13" \
  --release "$releases/$release13" --confirm >/dev/null 2>&1 && {
    echo "injected restore quarantine failure unexpectedly succeeded" >&2
    exit 1
  }
[[ $(sqlite3 "$database" 'PRAGMA user_version;') == 16 ]]
[[ $(sqlite3 "$database" 'SELECT value FROM marker;') == production-schema16 ]]
[[ $(readlink "$current") == releases/$release16 ]]

find "$backups" -maxdepth 1 -type f -name '*-manual.sqlite3.*' -delete
SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  SANGUINIUS_TEST_FAIL_RESTORE_ACTIVATION=true \
  "$repository/scripts/sanguinius-restore.bash" apply "$archive13" \
  --release "$releases/$release13" --confirm >/dev/null 2>&1 && {
    echo "injected restore activation failure unexpectedly succeeded" >&2
    exit 1
  }
[[ $(sqlite3 "$database" 'PRAGMA user_version;') == 16 ]]
[[ $(sqlite3 "$database" 'SELECT value FROM marker;') == production-schema16 ]]
[[ $(readlink "$current") == releases/$release16 ]]
[[ $(<"$state/state-version") == 16 ]]
grep -q 'Type=notify' "$unit"

find "$backups" -maxdepth 1 -type f -name '*-manual.sqlite3.*' -delete
SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  "$repository/scripts/sanguinius-restore.bash" apply "$archive13" \
  --release "$releases/$release13" --confirm >/dev/null
[[ $(sqlite3 "$database" 'PRAGMA user_version;') == 13 ]]
[[ $(sqlite3 "$database" 'SELECT value FROM marker;') == schema13-fixture ]]
[[ $(readlink "$current") == releases/$release13 ]]
[[ $(readlink "$previous") == releases/$release16 ]]
[[ $(<"$state/state-version") == 13 ]]
grep -q 'Type=simple' "$unit"
if grep -q '^invariants ' "$releases/$release13/invocations.log"; then
  echo "schema-13 restore invoked schema-16 umbrella invariants" >&2
  exit 1
fi
find "$backups" -maxdepth 1 -type f -name '*-schema16-*-manual.sqlite3.zst' | grep -q .

corrupt="$temporary/corrupt.sqlite3.zst"
cp "$archive13" "$corrupt"
cp "${archive13%.zst}.json" "${corrupt%.zst}.json"
cp "${archive13%.zst}.sha256" "${corrupt%.zst}.sha256"
printf corruption >>"$corrupt"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" verify "$corrupt" \
    --release "$releases/$release13" >/dev/null 2>&1; then
  echo "restore verification accepted a corrupted archive" >&2
  exit 1
fi
[[ $(sqlite3 "$database" 'SELECT value FROM marker;') == schema13-fixture ]]

echo "backup/restore shell tests passed"
