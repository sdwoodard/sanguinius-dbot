#!/usr/bin/env bash
set -euo pipefail
umask 0077

usage() {
  echo "Usage: $0 verify <archive> --release <release-directory>" >&2
  echo "       $0 apply <archive> --release <release-directory> --confirm" >&2
  exit 2
}

operation=${1:-}
archive=${2:-}
shift 2 2>/dev/null || usage
release=
confirmed=false
while (( $# )); do
  case "$1" in
    --release) (( $# >= 2 )) || usage; release=$2; shift 2 ;;
    --confirm) confirmed=true; shift ;;
    *) usage ;;
  esac
done
[[ $operation == verify || $operation == apply ]] || usage
[[ -n $archive && -n $release && $archive == /* && $release == /* ]] || usage
[[ $operation != apply || $confirmed == true ]] || usage

root=${SANGUINIUS_ROOT:-}
test_mode=false
if [[ -n $root ]]; then
  [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true && $EUID -ne 0 ]] || exit 2
  [[ $root == /* && $root != / && ! -L $root ]] || exit 2
  test_mode=true
fi
if [[ $operation == apply && $test_mode != true && $EUID -ne 0 ]]; then
  echo "Restore apply requires root." >&2
  exit 1
fi
if [[ $operation == apply && $test_mode != true ]]; then
  [[ $(hostname -s) == nuln ]] || {
    echo "Production restore apply is restricted to nuln." >&2
    exit 1
  }
  [[ $(dirname "$archive") == /var/backups/sanguinius &&
     $(dirname "$release") == /opt/sanguinius/releases &&
     $release != *'/../'* && $archive != *'/../'* ]] || {
    echo "Production restore paths are outside the managed layout." >&2
    exit 1
  }
fi

state="$root/var/lib/sanguinius"
runtime="$state/runtime"
database="$state/sanguinius.sqlite3"
lock_file="$runtime/operations.lock"
binary="$release/bin/sanguinius"
metadata=${archive%.zst}.json
checksum=${archive%.zst}.sha256
[[ -d $runtime && ! -L $runtime && -d $release && ! -L $release &&
   -x $binary && -f $archive &&
   -f $metadata && -f $checksum && ! -L $archive && ! -L $metadata &&
   ! -L $checksum ]] || {
  echo "Restore inputs are missing or unsafe." >&2
  exit 1
}

stage=$(mktemp -d "$runtime/restore.XXXXXXXX")
raw="$stage/restored.sqlite3"
installed=false
quarantine=
cleanup() {
  local exit_code=$?
  rm -f -- "$raw" "$raw-wal" "$raw-shm" "$raw-journal" 2>/dev/null || true
  rmdir -- "$stage" 2>/dev/null || true
  if [[ $exit_code -ne 0 && $installed == true && -n $quarantine ]]; then
    rm -f -- "$database" "$database-wal" "$database-shm" "$database-journal" 2>/dev/null || true
    for suffix in '' -wal -shm -journal; do
      [[ ! -e $quarantine/sanguinius.sqlite3$suffix ]] ||
        mv -T "$quarantine/sanguinius.sqlite3$suffix" "$database$suffix"
    done
  fi
  exit "$exit_code"
}
trap cleanup EXIT

archive_name=$(basename "$archive")
expected_name=$(sed -n 's/.*"archive":"\([A-Za-z0-9._+-]*\)".*/\1/p' "$metadata")
expected_sha=$(sed -n 's/.*"compressed_sha256":"\([0-9a-f]\{64\}\)".*/\1/p' "$metadata")
original_sha=$(sed -n 's/.*"original_sha256":"\([0-9a-f]\{64\}\)".*/\1/p' "$metadata")
schema=$(sed -n 's/.*"schema":\([0-9][0-9]*\).*/\1/p' "$metadata")
[[ $archive_name == "$expected_name" && $expected_sha =~ ^[0-9a-f]{64}$ &&
   $original_sha =~ ^[0-9a-f]{64}$ && $schema =~ ^[0-9]+$ ]] || {
  echo "Backup metadata is invalid." >&2
  exit 1
}
[[ $(sha256sum "$archive" | awk '{print $1}') == "$expected_sha" ]]
[[ $(awk 'NF == 2 {print $1}' "$checksum") == "$expected_sha" ]]
[[ $(awk 'NF == 2 {print $2}' "$checksum") == "$archive_name" ]]
zstd -q -t "$archive"
zstd -q -d -o "$raw" "$archive"
[[ $(sha256sum "$raw" | awk '{print $1}') == "$original_sha" ]]

release_metadata="$release/RELEASE-METADATA.json"
if [[ -f $release_metadata ]]; then
  release_schema=$(sed -n 's/.*"schema_target":\([0-9][0-9]*\).*/\1/p' "$release_metadata")
else
  release_json=$($binary --version --json)
  release_schema=$(sed -n 's/.*"schema_target":\([0-9][0-9]*\).*/\1/p' <<<"$release_json")
fi
[[ $release_schema == "$schema" ]] || {
  echo "Selected release is not schema-compatible with this backup." >&2
  exit 1
}

SANGUINIUS_DATABASE_FILE="$raw" "$binary" db integrity
SANGUINIUS_DATABASE_FILE="$raw" "$binary" db invariants check
SANGUINIUS_DATABASE_FILE="$raw" "$binary" db migrate
SANGUINIUS_DATABASE_FILE="$raw" "$binary" db check
if [[ $operation == verify ]]; then
  echo "restore=verified"
  echo "schema=$schema"
  exit 0
fi

if [[ $test_mode != true ]]; then
  if systemctl is-active --quiet sanguinius.service ||
     pgrep -x sanguinius >/dev/null 2>&1; then
    echo "Sanguinius must be inactive before restore." >&2
    exit 1
  fi
fi
exec 9>"$lock_file"
flock -n 9 || {
  echo "Another Sanguinius operation holds the lock." >&2
  exit 75
}
[[ -f $database && ! -L $database ]] || {
  echo "The production database path is invalid." >&2
  exit 1
}
if [[ $test_mode != true ]]; then
  for descriptor in /proc/[0-9]*/fd/[0-9]*; do
    [[ -e $descriptor ]] || continue
    open_path=$(readlink -f "$descriptor" 2>/dev/null || true)
    case "$open_path" in
      "$database"|"$database-wal"|"$database-shm"|"$database-journal")
        echo "The production database is open by another process." >&2
        exit 1 ;;
    esac
  done
fi

SANGUINIUS_LOCK_HELD=true "$release/libexec/sanguinius-backup.bash" manual --lock-held
stamp=$(date -u +%Y%m%dT%H%M%S%3NZ)
quarantine="$runtime/quarantine-$stamp"
mkdir -m 0700 "$quarantine"
for suffix in '' -wal -shm -journal; do
  [[ ! -e $database$suffix ]] || mv -T "$database$suffix" \
    "$quarantine/sanguinius.sqlite3$suffix"
done
installed=true
install -m 0600 "$raw" "$database"
if [[ $test_mode != true ]]; then
  chown sanguinius:sanguinius "$database"
fi
SANGUINIUS_DATABASE_FILE="$database" "$binary" db migrate
SANGUINIUS_DATABASE_FILE="$database" "$binary" db check
SANGUINIUS_DATABASE_FILE="$database" "$binary" db integrity
SANGUINIUS_DATABASE_FILE="$database" "$binary" db invariants check
installed=false
echo "restore=applied"
echo "quarantine=$(basename "$quarantine")"
