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
test_mode=false
if [[ -n $root ]]; then
  [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true && $EUID -ne 0 ]] || {
    echo "SANGUINIUS_ROOT is available only to unprivileged shell tests." >&2
    exit 2
  }
  [[ $root == /* && $root != / && ! -L $root ]] || exit 2
  test_mode=true
else
  [[ $EUID -eq 0 ]] || {
    echo "Production backups require root." >&2
    exit 1
  }
fi

state="$root/var/lib/sanguinius"
runtime="$state/runtime"
backup_directory="$root/var/backups/sanguinius"
release_input=${SANGUINIUS_BACKUP_RELEASE:-"$root/opt/sanguinius/current"}
release=$release_input
database="$state/sanguinius.sqlite3"
status_file="$runtime/operations-status.json"
lock_file="$runtime/operations.lock"

verify_database_filesystem_entries() {
  local candidate suffix links
  [[ -f $database && ! -L $database ]] || {
    echo "The production database path is unsafe." >&2
    return 1
  }
  links=$(stat -c %h -- "$database")
  [[ $links == 1 ]] || {
    echo "The production database has a hard-link alias." >&2
    return 1
  }
  for suffix in .lock -wal -shm -journal; do
    candidate="$database$suffix"
    if [[ -e $candidate || -L $candidate ]]; then
      [[ -f $candidate && ! -L $candidate ]] || {
        echo "A production database companion is unsafe." >&2
        return 1
      }
      links=$(stat -c %h -- "$candidate")
      [[ $links == 1 ]] || {
        echo "A production database companion has a hard-link alias." >&2
        return 1
      }
    fi
  done
}

for directory in "$state" "$runtime" "$backup_directory"; do
  [[ -d $directory && ! -L $directory ]] || {
    echo "Required backup directory is missing or unsafe." >&2
    exit 1
  }
done
if [[ $test_mode != true ]]; then
  if [[ $release_input == /opt/sanguinius/current ]]; then
    [[ -L $release_input ]] || {
      echo "The active release link is unavailable." >&2
      exit 1
    }
    release=$(readlink -f -- "$release_input")
  fi
  [[ $release == /opt/sanguinius/releases/* &&
     $(dirname "$release") == /opt/sanguinius/releases && ! -L $release ]] || {
    echo "Backup release override is outside the managed layout." >&2
    exit 1
  }
  [[ $(stat -c '%U:%G' "$release") == root:root &&
     -z $(find "$release" -xdev \
       \( ! -user root -o ! -group root -o -perm /022 \) -print -quit) ]] || {
    echo "Backup release ownership is unsafe." >&2
    exit 1
  }
  [[ $(stat -c '%U:%G:%a' "$runtime") == root:sanguinius:750 &&
     $(stat -c '%U:%G:%a' "$backup_directory") == root:sanguinius:710 ]] || {
    echo "Privileged backup directory ownership is unsafe." >&2
    exit 1
  }
else
  if [[ -L $release_input ]]; then
    release=$(readlink -f -- "$release_input")
  fi
  [[ $release == "$root/"* && $(dirname "$release") == "$root/opt/sanguinius/releases" ]] || exit 2
fi
binary="$release/bin/sanguinius"
[[ -x $binary ]] || {
  echo "Release binary or database is unavailable." >&2
  exit 1
}
verify_database_filesystem_entries

[[ ! -e $lock_file || (-f $lock_file && ! -L $lock_file) ]] || {
  echo "The operations lock is unsafe." >&2
  exit 1
}
if [[ ! -e $lock_file ]]; then
  install -m 0600 /dev/null "$lock_file"
  [[ $test_mode == true ]] || chown root:root "$lock_file"
fi
if [[ $test_mode != true && $(stat -c '%U:%G:%a' "$lock_file") != root:root:600 ]]; then
  echo "The operations lock ownership is unsafe." >&2
  exit 1
fi
if [[ $lock_held == true ]]; then
  [[ -e /proc/$$/fd/9 && $(readlink "/proc/$$/fd/9") == "$lock_file" ]] || {
    echo "The inherited operations lock is invalid." >&2
    exit 1
  }
else
  exec 9<>"$lock_file"
  flock -n 9 || {
    echo "Another Sanguinius operation holds the lock." >&2
    exit 75
  }
fi

recover_retention_stages() {
  local retention_directory retention_name retained_path retained_name retained_mode
  while IFS= read -r retention_directory; do
    retention_name=$(basename "$retention_directory")
    [[ $(dirname "$retention_directory") == "$backup_directory" &&
       $retention_name =~ ^\.retention\.[A-Za-z0-9]{8}$ &&
       -d $retention_directory && ! -L $retention_directory ]] || {
      echo "A managed retention recovery directory is unsafe." >&2
      return 1
    }
    if [[ $test_mode != true &&
          $(stat -c '%U:%G:%a' "$retention_directory") != root:root:700 ]]; then
      echo "A managed retention recovery directory has unsafe ownership." >&2
      return 1
    fi
    if [[ -n $(find -P "$retention_directory" -mindepth 1 -maxdepth 1 \
      ! -type f -print -quit) ]]; then
      echo "A managed retention recovery directory contains an unsafe entry." >&2
      return 1
    fi
    while IFS= read -r retained_path; do
      retained_name=$(basename "$retained_path")
      [[ $retained_name =~ ^[0-9]{8}T[0-9]{6}Z-schema[0-9]+-[0-9a-f]{12}-rolling\.sqlite3\.(zst|json|sha256)$ &&
         -f $retained_path && ! -L $retained_path ]] || {
        echo "A staged rolling-backup filename is unsafe." >&2
        return 1
      }
      if [[ $test_mode != true ]]; then
        retained_mode=$(stat -c %a "$retained_path")
        if [[ $(stat -c '%U:%G' "$retained_path") != root:root ]] ||
           (( (8#$retained_mode & 0077) != 0 )); then
          echo "A staged rolling-backup file has unsafe ownership." >&2
          return 1
        fi
      fi
      [[ ! -e $backup_directory/$retained_name &&
         ! -L $backup_directory/$retained_name ]] || {
        echo "A staged rolling backup conflicts with a published file." >&2
        return 1
      }
      mv -T "$retained_path" "$backup_directory/$retained_name"
    done < <(
      find -P "$retention_directory" -mindepth 1 -maxdepth 1 -type f \
        -print | LC_ALL=C sort
    )
    rmdir -- "$retention_directory"
  done < <(
    find -P "$backup_directory" -mindepth 1 -maxdepth 1 -type d \
      -name '.retention.????????' -print | LC_ALL=C sort
  )
}

stage=$(mktemp -d "$runtime/backup.XXXXXXXX")
raw="$stage/database.sqlite3"
restored="$stage/restored.sqlite3"
archive_tmp="$stage/archive.zst"
metadata_tmp="$stage/metadata.json"
checksum_tmp="$stage/archive.sha256"
completed=false
published=false
publication_started=false
retention_stage=
retention_committed=false
retention_restore_failed=false
declare -a retention_moved=()
backup_at_ms=
schema=
archive=
metadata=
checksum=

write_status() {
  local result=$1
  if [[ $result == succeeded && ${SANGUINIUS_SCRIPT_TESTING:-false} == true &&
        ${SANGUINIUS_TEST_FAIL_BACKUP_STATUS:-false} == true ]]; then
    echo "Injected backup status-publication failure." >&2
    return 1
  fi
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
  local retained_name
  if [[ $retention_committed != true && -n $retention_stage &&
        -d $retention_stage && ! -L $retention_stage ]]; then
    for retained_name in "${retention_moved[@]}"; do
      if [[ -f $retention_stage/$retained_name ]]; then
        if [[ -e $backup_directory/$retained_name ||
              -L $backup_directory/$retained_name ]] ||
           ! mv -T "$retention_stage/$retained_name" \
             "$backup_directory/$retained_name" 2>/dev/null; then
          retention_restore_failed=true
        fi
      fi
    done
  fi
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
  if [[ $retention_restore_failed != true && -n $retention_stage &&
        -d $retention_stage &&
        ! -L $retention_stage ]]; then
    find -P "$retention_stage" -xdev -depth -delete 2>/dev/null || true
  fi
  rmdir -- "$stage" 2>/dev/null || true
  exit "$exit_code"
}
trap cleanup EXIT

recover_retention_stages

release_metadata="$release/RELEASE-METADATA.json"
[[ -f $release_metadata && ! -L $release_metadata ]] || {
  echo "Release metadata is unavailable." >&2
  exit 1
}
revision=$(sed -n 's/.*"revision":"\([0-9a-f]\{40\}\)".*/\1/p' \
  "$release_metadata")
release_id=$(sed -n 's/.*"release_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' \
  "$release_metadata")
deployment_id=$(sed -n \
  's/.*"deployment_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' \
  "$release_metadata")
[[ -n $deployment_id ]] || deployment_id=$release_id
release_schema=$(sed -n 's/.*"schema_target":\([0-9][0-9]*\).*/\1/p' \
  "$release_metadata")
compatibility=$(sed -n \
  's/.*"compatibility_release":\(true\|false\).*/\1/p' \
  "$release_metadata")
[[ $revision =~ ^[0-9a-f]{40}$ &&
   $release_id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
   $deployment_id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
   $release_schema =~ ^[0-9]+$ &&
   ($compatibility == true || $compatibility == false) ]] || {
  echo "Release identity is invalid." >&2
  exit 1
}
if [[ $(basename "$release") != "$deployment_id" ]]; then
  echo "Release directory and metadata disagree." >&2
  exit 1
fi
if [[ $compatibility == false ]]; then
  version=$(sed -n 's/.*"version":"\([0-9.]*\)".*/\1/p' \
    "$release_metadata")
  catalog=$(sed -n \
    's/.*"command_catalog_version":\([0-9][0-9]*\).*/\1/p' \
    "$release_metadata")
  version_json=$("$binary" --version --json)
  [[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && $catalog =~ ^[0-9]+$ &&
     $version_json == *'"release_id":"'"$release_id"'"'* &&
     $version_json == *'"version":"'"$version"'"'* &&
     $version_json == *'"revision":"'"$revision"'"'* &&
     $version_json == *'"schema_target":'"$release_schema"* &&
     $version_json == *'"command_catalog_version":'"$catalog"* ]] || {
    echo "Release binary and metadata disagree." >&2
    exit 1
  }
fi

verify_database_filesystem_entries
status=$(SANGUINIUS_DATABASE_FILE="$database" "$binary" db status)
schema=$(sed -n 's/^current_schema=\([0-9][0-9]*\)$/\1/p' <<<"$status")
target=$(sed -n 's/^target_schema=\([0-9][0-9]*\)$/\1/p' <<<"$status")
[[ $schema =~ ^[0-9]+$ && $schema == "$target" &&
   $target == "$release_schema" ]] || {
  echo "The source database schema is not current for this release." >&2
  exit 1
}

verify_database_filesystem_entries
SANGUINIUS_DATABASE_FILE="$database" "$binary" db backup "$raw"
for candidate in "$database" "$raw"; do
  SANGUINIUS_DATABASE_FILE="$candidate" "$binary" db integrity
  if (( schema >= 16 )); then
    SANGUINIUS_DATABASE_FILE="$candidate" "$binary" db invariants check
  else
    SANGUINIUS_DATABASE_FILE="$candidate" "$binary" db relationships check
    SANGUINIUS_DATABASE_FILE="$candidate" "$binary" db tarot check
  fi
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
if (( schema >= 16 )); then
  SANGUINIUS_DATABASE_FILE="$restored" "$binary" db invariants check
else
  SANGUINIUS_DATABASE_FILE="$restored" "$binary" db relationships check
  SANGUINIUS_DATABASE_FILE="$restored" "$binary" db tarot check
fi
SANGUINIUS_DATABASE_FILE="$restored" "$binary" db migrate
SANGUINIUS_DATABASE_FILE="$restored" "$binary" db check

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
backup_at_ms=$(date -u +%s%3N)
base="$timestamp-schema$schema-${revision:0:12}-$backup_class.sqlite3"
archive="$backup_directory/$base.zst"
metadata="$backup_directory/$base.json"
checksum="$backup_directory/$base.sha256"
[[ ! -e $archive && ! -L $archive &&
   ! -e $metadata && ! -L $metadata &&
   ! -e $checksum && ! -L $checksum ]] || {
  echo "Backup destination already exists." >&2
  exit 1
}

printf '{"format_version":1,"archive":"%s","class":"%s","created_at_ms":%s,"schema":%s,"revision":"%s","release_id":"%s","deployment_id":"%s","original_sha256":"%s","compressed_sha256":"%s","original_size":%s,"compressed_size":%s,"integrity":"ok","foreign_keys":"ok","domain_invariants":"ok","restore_copy":"ok"}\n' \
  "$base.zst" "$backup_class" "$backup_at_ms" "$schema" "$revision" \
  "$release_id" "$deployment_id" "$original_sha" "$compressed_sha" \
  "$original_size" "$compressed_size" >"$metadata_tmp"
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
  rolling_metadata=()
  while IFS= read -r old_metadata; do
    [[ $old_metadata =~ ^[0-9]{8}T[0-9]{6}Z-schema[0-9]+-[0-9a-f]{12}-rolling\.sqlite3\.json$ ]] || continue
    old_base=${old_metadata%.json}
    [[ -f $backup_directory/$old_base.zst &&
       ! -L $backup_directory/$old_base.zst &&
       -f $backup_directory/$old_base.sha256 &&
       ! -L $backup_directory/$old_base.sha256 ]] || continue
    rolling_metadata+=("$old_metadata")
  done < <(
    find "$backup_directory" -maxdepth 1 -type f \
      -name '*-rolling.sqlite3.json' -printf '%f\n' | LC_ALL=C sort -r
  )
  if (( ${#rolling_metadata[@]} > 7 )); then
    retention_stage=$(mktemp -d "$backup_directory/.retention.XXXXXXXX")
    chmod 0700 "$retention_stage"
    for old_metadata in "${rolling_metadata[@]:7}"; do
      old_base=${old_metadata%.json}
      for retained_name in "$old_base.zst" "$old_base.json" \
                           "$old_base.sha256"; do
        mv -T "$backup_directory/$retained_name" \
          "$retention_stage/$retained_name"
        retention_moved+=("$retained_name")
      done
    done
  fi
fi

write_status succeeded
completed=true
retention_committed=true
if [[ -n $retention_stage && -d $retention_stage &&
      ! -L $retention_stage ]]; then
  find -P "$retention_stage" -xdev -depth -delete 2>/dev/null || true
fi
echo "backup=verified"
echo "schema=$schema"
echo "class=$backup_class"
echo "archive=$base.zst"
