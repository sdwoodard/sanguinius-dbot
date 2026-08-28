#!/usr/bin/env bash
set -euo pipefail
umask 0077

usage() {
  echo "Usage: $0 <rolling|pre-migration|failure|manual> [--lock-held]" >&2
  exit 2
}

backup_class=${1:-}
shift || true
case "$backup_class" in
  rolling|pre-migration|failure|manual) ;;
  *) usage ;;
esac
lock_held=false
if [[ ${1:-} == --lock-held && $# -eq 1 ]]; then
  lock_held=true
elif (( $# != 0 )); then
  usage
fi

root=${SANGUINIUS_ROOT:-}
if [[ -n $root ]]; then
  [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true && $EUID -ne 0 ]] || {
    echo "SANGUINIUS_ROOT is available only to unprivileged shell tests." >&2
    exit 2
  }
  [[ $root == /* && $root != / && ! -L $root ]] || exit 2
else
  [[ $EUID -eq 0 ]] || {
    echo "Production backups require root." >&2
    exit 1
  }
fi

state="$root/var/lib/sanguinius"
runtime="$state/runtime"
backup_directory="$root/var/backups/sanguinius"
release="$root/opt/sanguinius/current"
database="$state/sanguinius.sqlite3"
status_file="$runtime/operations-status.json"
lock_file="$runtime/operations.lock"
binary="$release/bin/sanguinius"

for directory in "$state" "$runtime" "$backup_directory"; do
  [[ -d $directory && ! -L $directory ]] || {
    echo "Required backup directory is missing or unsafe." >&2
    exit 1
  }
done
[[ -x $binary && -f $database && ! -L $database ]] || {
  echo "Release binary or database is unavailable." >&2
  exit 1
}

if [[ $lock_held == true ]]; then
  [[ -e /proc/$$/fd/9 && $(readlink "/proc/$$/fd/9") == "$lock_file" ]] || {
    echo "The inherited operations lock is invalid." >&2
    exit 1
  }
else
  exec 9>"$lock_file"
  flock -n 9 || {
    echo "Another Sanguinius operation holds the lock." >&2
    exit 75
  }
fi

stage=$(mktemp -d "$runtime/backup.XXXXXXXX")
raw="$stage/database.sqlite3"
restored="$stage/restored.sqlite3"
archive_tmp="$stage/archive.zst"
metadata_tmp="$stage/metadata.json"
checksum_tmp="$stage/archive.sha256"
completed=false
published=false
publication_started=false
backup_at_ms=
schema=
archive=
metadata=
checksum=

write_status() {
  local result=$1
  local now_ms
  now_ms=$(date -u +%s%3N)
  local backup_result=failed
  local backup_stamp=0 backup_status_schema=0
  if [[ $result == succeeded ]]; then
    backup_result=succeeded
    backup_stamp=$backup_at_ms
    backup_status_schema=$schema
  elif [[ -f $status_file && ! -L $status_file ]]; then
    backup_stamp=$(sed -n 's/.*"backup_at_ms":\([0-9]*\).*/\1/p' \
      "$status_file")
    backup_status_schema=$(sed -n 's/.*"backup_schema":\([0-9]*\).*/\1/p' \
      "$status_file")
  fi
  [[ $backup_stamp =~ ^[0-9]+$ ]] || backup_stamp=0
  [[ $backup_status_schema =~ ^[0-9]+$ ]] || backup_status_schema=0
  local temporary="$stage/operations-status.json"
  printf '{"version":1,"updated_at_ms":%s,"result":"%s","backup_at_ms":%s,"backup_schema":%s,"backup_result":"%s"}\n' \
    "$now_ms" "$result" "$backup_stamp" "$backup_status_schema" \
    "$backup_result" >"$temporary"
  chmod 0644 "$temporary"
  mv -fT "$temporary" "$status_file"
  chmod 0644 "$status_file"
  chown root:root "$status_file" 2>/dev/null || true
}

cleanup() {
  local exit_code=$?
  if [[ $completed != true ]]; then
    write_status failed || true
  fi
  if [[ $publication_started == true && $published != true ]]; then
    for destination in "$archive" "$metadata" "$checksum"; do
      [[ -n $destination && $destination == "$backup_directory/"* ]] || continue
      rm -f -- "$destination"
    done
  fi
  for file in "$raw" "$restored" "$archive_tmp" "$metadata_tmp" \
              "$checksum_tmp" "$stage/operations-status.json"; do
    [[ ! -e $file || -f $file ]] && rm -f -- "$file"
  done
  rmdir -- "$stage" 2>/dev/null || true
  exit "$exit_code"
}
trap cleanup EXIT

version_json=$($binary --version --json)
revision=$(sed -n 's/.*"revision":"\([0-9a-f]\{40\}\)".*/\1/p' <<<"$version_json")
release_id=$(sed -n 's/.*"release_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' <<<"$version_json")
[[ $revision =~ ^[0-9a-f]{40}$ && $release_id =~ ^[A-Za-z0-9._+-]+$ ]] || {
  echo "Release identity is invalid." >&2
  exit 1
}

status=$(SANGUINIUS_DATABASE_FILE="$database" "$binary" db status)
schema=$(sed -n 's/^current_schema=\([0-9][0-9]*\)$/\1/p' <<<"$status")
target=$(sed -n 's/^target_schema=\([0-9][0-9]*\)$/\1/p' <<<"$status")
[[ $schema =~ ^[0-9]+$ && $schema == "$target" ]] || {
  echo "The source database schema is not current for this release." >&2
  exit 1
}

SANGUINIUS_DATABASE_FILE="$database" "$binary" db backup "$raw"
for candidate in "$database" "$raw"; do
  SANGUINIUS_DATABASE_FILE="$candidate" "$binary" db integrity
  SANGUINIUS_DATABASE_FILE="$candidate" "$binary" db invariants check
done

original_sha=$(sha256sum "$raw" | awk '{print $1}')
original_size=$(stat -c %s "$raw")
zstd -q -19 --threads=1 -o "$archive_tmp" "$raw"
zstd -q -t "$archive_tmp"
compressed_sha=$(sha256sum "$archive_tmp" | awk '{print $1}')
compressed_size=$(stat -c %s "$archive_tmp")

zstd -q -d -o "$restored" "$archive_tmp"
[[ $(sha256sum "$restored" | awk '{print $1}') == "$original_sha" ]]
SANGUINIUS_DATABASE_FILE="$restored" "$binary" db integrity
SANGUINIUS_DATABASE_FILE="$restored" "$binary" db invariants check
SANGUINIUS_DATABASE_FILE="$restored" "$binary" db migrate
SANGUINIUS_DATABASE_FILE="$restored" "$binary" db check

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
backup_at_ms=$(date -u +%s%3N)
base="$timestamp-schema$schema-${revision:0:12}-$backup_class.sqlite3"
archive="$backup_directory/$base.zst"
metadata="$backup_directory/$base.json"
checksum="$backup_directory/$base.sha256"
[[ ! -e $archive && ! -e $metadata && ! -e $checksum ]] || {
  echo "Backup destination already exists." >&2
  exit 1
}

printf '{"format_version":1,"archive":"%s","class":"%s","created_at_ms":%s,"schema":%s,"revision":"%s","release_id":"%s","original_sha256":"%s","compressed_sha256":"%s","original_size":%s,"compressed_size":%s,"integrity":"ok","foreign_keys":"ok","domain_invariants":"ok","restore_copy":"ok"}\n' \
  "$base.zst" "$backup_class" "$backup_at_ms" "$schema" "$revision" \
  "$release_id" "$original_sha" "$compressed_sha" "$original_size" \
  "$compressed_size" >"$metadata_tmp"
printf '%s  %s\n' "$compressed_sha" "$base.zst" >"$checksum_tmp"
publication_started=true
install -m 0600 "$archive_tmp" "$archive"
if [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true &&
      ${SANGUINIUS_TEST_FAIL_BACKUP_PUBLICATION:-false} == true ]]; then
  echo "Injected backup publication failure." >&2
  exit 1
fi
install -m 0600 "$metadata_tmp" "$metadata"
install -m 0600 "$checksum_tmp" "$checksum"
published=true

if [[ $backup_class == rolling ]]; then
  mapfile -t rolling_metadata < <(
    find "$backup_directory" -maxdepth 1 -type f \
      -name '*-rolling.sqlite3.json' -printf '%f\n' | LC_ALL=C sort -r
  )
  if (( ${#rolling_metadata[@]} > 7 )); then
    for old_metadata in "${rolling_metadata[@]:7}"; do
      [[ $old_metadata =~ ^[0-9]{8}T[0-9]{6}Z-schema[0-9]+-[0-9a-f]{12}-rolling\.sqlite3\.json$ ]] || continue
      old_base=${old_metadata%.json}
      [[ -f $backup_directory/$old_base.zst &&
         -f $backup_directory/$old_base.sha256 ]] || continue
      rm -f -- "$backup_directory/$old_base.zst" \
        "$backup_directory/$old_base.json" \
        "$backup_directory/$old_base.sha256"
    done
  fi
fi

write_status succeeded
completed=true
echo "backup=verified"
echo "schema=$schema"
echo "class=$backup_class"
echo "archive=$base.zst"
