#!/usr/bin/env bash
# shellcheck disable=SC2016
set -euo pipefail
umask 0077

repository=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
temporary=$(mktemp -d /tmp/sanguinius-backup-test.XXXXXXXX)
cleanup() {
  find "$temporary" -type f -delete
  find "$temporary" -type l -delete
  find "$temporary" -depth -type d -empty -delete
}
trap cleanup EXIT

for script in "$repository/scripts/sanguinius-backup.bash" \
              "$repository/scripts/sanguinius-restore.bash"; do
  bash -n "$script"
done

mkdir -p "$temporary/var/lib/sanguinius/runtime" \
  "$temporary/var/backups/sanguinius" \
  "$temporary/opt/sanguinius/releases/test/bin" \
  "$temporary/opt/sanguinius/releases/test/libexec"
ln -s releases/test "$temporary/opt/sanguinius/current"
printf 'schema16 fixture\n' >"$temporary/var/lib/sanguinius/sanguinius.sqlite3"

fake="$temporary/opt/sanguinius/releases/test/bin/sanguinius"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'set -euo pipefail' \
  'if [[ ${1:-} == --version ]]; then echo '\''{"revision":"0123456789abcdef0123456789abcdef01234567","release_id":"shell-test","schema_target":16}'\''; exit; fi' \
  'if [[ ${1:-} != db ]]; then exit 2; fi' \
  'case ${2:-} in' \
  '  status) printf '\''database=current\ncurrent_schema=16\ntarget_schema=16\npending_migrations=0\nsqlite=test\n'\'' ;;' \
  '  backup) if [[ -f $(dirname "$0")/../fail-backup ]]; then printf partial >"$3"; exit 1; fi; cp "$SANGUINIUS_DATABASE_FILE" "$3" ;;' \
  '  migrate) if [[ -f $(dirname "$0")/../fail-migrate ]]; then exit 1; fi; echo ok ;;' \
  '  check) if [[ -f $(dirname "$0")/../fail-production-check && $SANGUINIUS_DATABASE_FILE == */var/lib/sanguinius/sanguinius.sqlite3 ]]; then exit 1; fi; echo ok ;;' \
  '  integrity) echo ok ;;' \
  '  invariants) [[ ${3:-} == check ]]; echo invariants=ok ;;' \
  '  *) exit 2 ;;' \
  'esac' >"$fake"
chmod 0755 "$fake"
cp "$repository/scripts/sanguinius-backup.bash" \
  "$temporary/opt/sanguinius/releases/test/libexec/sanguinius-backup.bash"
chmod 0755 "$temporary/opt/sanguinius/releases/test/libexec/sanguinius-backup.bash"
printf '{"schema_target":16}\n' \
  >"$temporary/opt/sanguinius/releases/test/RELEASE-METADATA.json"

exec 8>"$temporary/var/lib/sanguinius/runtime/operations.lock"
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
archive=$(find "$temporary/var/backups/sanguinius" -type f \
  -name '*-rolling.sqlite3.zst' -print -quit)
[[ -n $archive && -f ${archive%.zst}.json && -f ${archive%.zst}.sha256 ]]
SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  "$repository/scripts/sanguinius-restore.bash" verify "$archive" \
  --release "$temporary/opt/sanguinius/releases/test" >/dev/null

mkdir "$temporary/restore-input"
restore_archive="$temporary/restore-input/$(basename "$archive")"
mv "$archive" "$restore_archive"
mv "${archive%.zst}.json" "${restore_archive%.zst}.json"
mv "${archive%.zst}.sha256" "${restore_archive%.zst}.sha256"
archive=$restore_archive

printf 'production changed before restore\n' \
  >"$temporary/var/lib/sanguinius/sanguinius.sqlite3"
SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  "$repository/scripts/sanguinius-restore.bash" apply "$archive" \
  --release "$temporary/opt/sanguinius/releases/test" --confirm >/dev/null
grep -q 'schema16 fixture' "$temporary/var/lib/sanguinius/sanguinius.sqlite3"
grep -R -q 'production changed before restore' \
  "$temporary/var/lib/sanguinius/runtime/quarantine-"*

find "$temporary/var/backups/sanguinius" -maxdepth 1 -type f \
  -name '*-manual.sqlite3.*' -delete
printf 'production retained after failed apply\n' \
  >"$temporary/var/lib/sanguinius/sanguinius.sqlite3"
touch "$temporary/opt/sanguinius/releases/test/fail-production-check"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" apply "$archive" \
    --release "$temporary/opt/sanguinius/releases/test" --confirm \
    >/dev/null 2>&1; then
  echo "restore apply ignored a post-install verification failure" >&2
  exit 1
fi
grep -q 'production retained after failed apply' \
  "$temporary/var/lib/sanguinius/sanguinius.sqlite3"
rm "$temporary/opt/sanguinius/releases/test/fail-production-check"

if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" apply "$archive" \
    --release "$temporary/opt/sanguinius/releases/test" >/dev/null 2>&1; then
  echo "restore apply did not require its confirmation guard" >&2
  exit 1
fi

touch "$temporary/opt/sanguinius/releases/test/fail-migrate"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" verify "$archive" \
    --release "$temporary/opt/sanguinius/releases/test" >/dev/null 2>&1; then
  echo "restore verification ignored a migration failure" >&2
  exit 1
fi
rm "$temporary/opt/sanguinius/releases/test/fail-migrate"

printf '{"schema_target":15}\n' \
  >"$temporary/opt/sanguinius/releases/test/RELEASE-METADATA.json"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" verify "$archive" \
    --release "$temporary/opt/sanguinius/releases/test" >/dev/null 2>&1; then
  echo "restore verification accepted an incompatible release" >&2
  exit 1
fi
printf '{"schema_target":16}\n' \
  >"$temporary/opt/sanguinius/releases/test/RELEASE-METADATA.json"

ln -s "$archive" "$temporary/restore-input/archive-link.zst"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" verify \
    "$temporary/restore-input/archive-link.zst" \
    --release "$temporary/opt/sanguinius/releases/test" >/dev/null 2>&1; then
  echo "restore verification followed an archive symlink" >&2
  exit 1
fi

for index in 01 02 03 04 05 06 07 08; do
  base="202001${index}T000000Z-schema16-0123456789ab-rolling.sqlite3"
  printf old >"$temporary/var/backups/sanguinius/$base.zst"
  printf old >"$temporary/var/backups/sanguinius/$base.json"
  printf old >"$temporary/var/backups/sanguinius/$base.sha256"
done
printf preserve >"$temporary/var/backups/sanguinius/legacy-backup.zst"
printf preserve >"$temporary/var/backups/sanguinius/unrecognized-rolling.sqlite3.zst"
printf preserve >"$temporary/var/backups/sanguinius/keep-manual.sqlite3.json"
SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
  "$repository/scripts/sanguinius-backup.bash" rolling >/dev/null
rolling_count=$(find "$temporary/var/backups/sanguinius" -maxdepth 1 \
  -type f -name '*-rolling.sqlite3.json' | wc -l)
[[ $rolling_count -eq 7 ]]
[[ -f $temporary/var/backups/sanguinius/legacy-backup.zst ]]
[[ -f $temporary/var/backups/sanguinius/unrecognized-rolling.sqlite3.zst ]]
[[ -f $temporary/var/backups/sanguinius/keep-manual.sqlite3.json ]]

artifact_count=$(find "$temporary/var/backups/sanguinius" -maxdepth 1 \
  -type f | wc -l)
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    SANGUINIUS_TEST_FAIL_BACKUP_PUBLICATION=true \
    "$repository/scripts/sanguinius-backup.bash" failure >/dev/null 2>&1; then
  echo "injected backup publication failure unexpectedly succeeded" >&2
  exit 1
fi
[[ $(find "$temporary/var/backups/sanguinius" -maxdepth 1 -type f | wc -l) \
   -eq $artifact_count ]]

touch "$temporary/opt/sanguinius/releases/test/fail-backup"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-backup.bash" failure >/dev/null 2>&1; then
  echo "interrupted backup unexpectedly succeeded" >&2
  exit 1
fi
rm "$temporary/opt/sanguinius/releases/test/fail-backup"
[[ $(find "$temporary/var/backups/sanguinius" -maxdepth 1 -type f | wc -l) \
   -eq $artifact_count ]]
grep -q '"backup_result":"failed"' \
  "$temporary/var/lib/sanguinius/runtime/operations-status.json"

printf 'corruption\n' >>"$archive"
if SANGUINIUS_ROOT="$temporary" SANGUINIUS_SCRIPT_TESTING=true \
    "$repository/scripts/sanguinius-restore.bash" verify "$archive" \
    --release "$temporary/opt/sanguinius/releases/test" >/dev/null 2>&1; then
  echo "restore verification accepted a corrupted archive" >&2
  exit 1
fi
grep -q 'production retained after failed apply' \
  "$temporary/var/lib/sanguinius/sanguinius.sqlite3"

echo "backup/restore shell tests passed"
