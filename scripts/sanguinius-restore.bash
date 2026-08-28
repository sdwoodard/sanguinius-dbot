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

script_directory=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
backup_helper="$script_directory/sanguinius-backup.bash"
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
if [[ $test_mode == true ]]; then
  [[ $(dirname "$release") == "$root/opt/sanguinius/releases" &&
     $archive == "$root/"* ]] || exit 2
fi

state="$root/var/lib/sanguinius"
runtime="$state/runtime"
database="$state/sanguinius.sqlite3"
lock_file="$runtime/operations.lock"
current_link="$root/opt/sanguinius/current"
previous_link="$root/opt/sanguinius/previous"
unit_path="$root/etc/systemd/system/sanguinius.service"
state_version="$state/state-version"
binary="$release/bin/sanguinius"
metadata=${archive%.zst}.json
checksum=${archive%.zst}.sha256

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
[[ -d $runtime && ! -L $runtime && -d $release && ! -L $release &&
   -x $binary && -f $archive && -f $metadata && -f $checksum &&
   ! -L $archive && ! -L $metadata && ! -L $checksum ]] || {
  echo "Restore inputs are missing or unsafe." >&2
  exit 1
}
if [[ $test_mode != true && $operation == apply ]]; then
  [[ $(stat -c '%U:%G:%a' "$runtime") == root:sanguinius:750 ]] || {
    echo "The privileged runtime directory is unsafe." >&2
    exit 1
  }
  [[ $(stat -c '%U:%G' "$release") == root:root &&
     -z $(find "$release" -xdev \
       \( ! -user root -o ! -group root -o -perm /022 \) -print -quit) ]] || {
    echo "The selected release ownership is unsafe." >&2
    exit 1
  }
  for restore_input in "$archive" "$metadata" "$checksum"; do
    restore_mode=$(stat -c %a "$restore_input")
    if [[ $(stat -c '%U:%G' "$restore_input") != root:root ]] ||
       (( (8#$restore_mode & 0022) != 0 )); then
      echo "A restore artifact has unsafe ownership or permissions." >&2
      exit 1
    fi
  done
fi

stage=$(mktemp -d "$runtime/restore.XXXXXXXX")
raw="$stage/restored.sqlite3"
ready="$stage/database.ready"
quarantine=
database_switched=false
activation_started=false
old_current=
old_previous=
had_previous=false
had_state_version=false
had_unit=false
unit_temporary=

atomic_link() {
  local target=$1 destination=$2 temporary
  temporary="$(dirname "$destination")/.restore-link.$$.$RANDOM"
  [[ ! -e $temporary && ! -L $temporary ]] || return 1
  ln -s -- "$target" "$temporary"
  mv -Tf -- "$temporary" "$destination"
}

restore_activation() {
  local rollback_failed=false
  if [[ -n $old_current ]]; then
    if [[ $test_mode == true &&
          ${SANGUINIUS_TEST_FAIL_ACTIVATION_ROLLBACK:-false} == true ]]; then
      rollback_failed=true
    else
      atomic_link "$old_current" "$current_link" || rollback_failed=true
    fi
  fi
  if [[ $had_previous == true ]]; then
    atomic_link "$old_previous" "$previous_link" || rollback_failed=true
  elif [[ -L $previous_link ]]; then
    unlink -- "$previous_link" || rollback_failed=true
  fi
  if [[ $had_state_version == true ]]; then
    if install -m 0600 "$stage/original-state-version" \
        "$stage/state-version.rollback"; then
      mv -Tf -- "$stage/state-version.rollback" "$state_version" ||
        rollback_failed=true
    else
      rollback_failed=true
    fi
  elif [[ -f $state_version && ! -L $state_version ]]; then
    rm -f -- "$state_version" || rollback_failed=true
  fi
  if [[ $had_unit == true ]]; then
    unit_temporary="$(dirname "$unit_path")/.sanguinius.service.rollback.$$"
    if [[ -e $unit_temporary || -L $unit_temporary ]]; then
      rollback_failed=true
    elif install -m 0644 "$stage/original.service" "$unit_temporary"; then
      if mv -Tf -- "$unit_temporary" "$unit_path"; then
        unit_temporary=
      else
        rollback_failed=true
      fi
    else
      rollback_failed=true
    fi
  elif [[ -f $unit_path && ! -L $unit_path ]]; then
    rm -f -- "$unit_path" || rollback_failed=true
  fi
  if [[ $test_mode != true ]]; then
    systemctl daemon-reload >/dev/null 2>&1 || rollback_failed=true
  fi
  [[ -L $current_link && $(readlink -- "$current_link") == "$old_current" ]] ||
    rollback_failed=true
  if [[ $had_previous == true ]]; then
    [[ -L $previous_link &&
       $(readlink -- "$previous_link") == "$old_previous" ]] ||
      rollback_failed=true
  else
    [[ ! -e $previous_link && ! -L $previous_link ]] || rollback_failed=true
  fi
  if [[ $had_state_version == true ]]; then
    [[ -f $state_version && ! -L $state_version &&
       $(stat -c %a "$state_version") == 600 ]] &&
      cmp -s "$stage/original-state-version" "$state_version" ||
      rollback_failed=true
  else
    [[ ! -e $state_version && ! -L $state_version ]] || rollback_failed=true
  fi
  if [[ $had_unit == true ]]; then
    [[ -f $unit_path && ! -L $unit_path &&
       $(stat -c %a "$unit_path") == 644 ]] &&
      cmp -s "$stage/original.service" "$unit_path" || rollback_failed=true
  else
    [[ ! -e $unit_path && ! -L $unit_path ]] || rollback_failed=true
  fi
  [[ $rollback_failed == false ]]
}

fence_service_after_restore_rollback_failure() {
  if [[ $test_mode == true ]]; then
    install -m 0600 /dev/null "$stage/service-fenced"
    return
  fi
  local fence_failed=false active_state enabled_state processes
  systemctl stop sanguinius.service || fence_failed=true
  active_state=$(systemctl is-active sanguinius.service 2>/dev/null || true)
  processes=$(pgrep -x sanguinius 2>/dev/null || true)
  systemctl disable sanguinius.service || fence_failed=true
  enabled_state=$(systemctl is-enabled sanguinius.service 2>/dev/null || true)
  [[ ($active_state == inactive || $active_state == failed) &&
     -z $processes && $enabled_state == disabled ]] || fence_failed=true
  [[ $fence_failed == false ]]
}

cleanup() {
  local exit_code=$?
  set +e
  local expected_hash marker suffix
  local activation_rollback_failed=false database_rollback_failed=false
  if [[ $exit_code -ne 0 && $activation_started == true ]]; then
    if ! restore_activation; then
      activation_rollback_failed=true
      echo "CRITICAL: The prior release activation could not be fully restored; fencing the service." >&2
      fence_service_after_restore_rollback_failure ||
        echo "CRITICAL: Service fencing after activation rollback failure could not be verified." >&2
    fi
  fi
  if [[ $exit_code -ne 0 && $database_switched == true && -n $quarantine ]]; then
    for suffix in '' -wal -shm -journal; do
      marker="$stage/original${suffix:-database}"
      if [[ -e $quarantine/sanguinius.sqlite3$suffix ]]; then
        if ! rm -f -- "$database$suffix"; then
          database_rollback_failed=true
          continue
        fi
        if [[ $suffix == '' && $test_mode == true &&
              ${SANGUINIUS_TEST_FAIL_DATABASE_ROLLBACK:-false} == true ]]; then
          database_rollback_failed=true
          continue
        fi
        mv -T "$quarantine/sanguinius.sqlite3$suffix" "$database$suffix" ||
          database_rollback_failed=true
      elif [[ -e $marker || -L $marker ]]; then
        database_rollback_failed=true
      elif ! rm -f -- "$database$suffix"; then
        database_rollback_failed=true
      fi
    done
    for suffix in '' -wal -shm -journal; do
      marker="$stage/original${suffix:-database}"
      if [[ -f $marker && ! -L $marker ]]; then
        expected_hash=$(<"$marker")
        [[ $expected_hash =~ ^[0-9a-f]{64}$ &&
           -f $database$suffix && ! -L $database$suffix &&
           $(sha256sum "$database$suffix" | awk '{print $1}') == \
             "$expected_hash" &&
           ! -e $quarantine/sanguinius.sqlite3$suffix &&
           ! -L $quarantine/sanguinius.sqlite3$suffix ]] ||
          database_rollback_failed=true
      else
        [[ ! -e $marker && ! -L $marker &&
           ! -e $database$suffix && ! -L $database$suffix ]] ||
          database_rollback_failed=true
      fi
    done
    if [[ $database_rollback_failed == true ]]; then
      echo "CRITICAL: The original production database could not be fully restored; fencing the service." >&2
      fence_service_after_restore_rollback_failure ||
        echo "CRITICAL: Service fencing after database rollback failure could not be verified." >&2
    fi
  fi
  if [[ -n $unit_temporary && $unit_temporary == "$(dirname "$unit_path")/.sanguinius.service."* &&
        -f $unit_temporary && ! -L $unit_temporary ]]; then
    rm -f -- "$unit_temporary"
  fi
  if [[ $exit_code -ne 0 && $database_rollback_failed == false &&
        -n $quarantine && -d $quarantine &&
        ! -L $quarantine ]]; then
    rmdir -- "$quarantine" 2>/dev/null || true
  fi
  if [[ $activation_rollback_failed == true ||
        $database_rollback_failed == true ]]; then
    echo "CRITICAL: Privileged restore diagnostics were retained for operator recovery." >&2
  else
    find -P "$stage" -maxdepth 1 -type f -delete 2>/dev/null || true
    rmdir -- "$stage" 2>/dev/null || true
  fi
  exit "$exit_code"
}
trap cleanup EXIT

run_domain_checks() {
  local candidate=$1 candidate_schema=$2 candidate_binary=$3
  SANGUINIUS_DATABASE_FILE="$candidate" "$candidate_binary" db integrity
  if (( candidate_schema >= 16 )); then
    SANGUINIUS_DATABASE_FILE="$candidate" "$candidate_binary" db invariants check
  else
    SANGUINIUS_DATABASE_FILE="$candidate" "$candidate_binary" db relationships check
    SANGUINIUS_DATABASE_FILE="$candidate" "$candidate_binary" db tarot check
  fi
}

release_schema() {
  local candidate_metadata="$1/RELEASE-METADATA.json"
  [[ -f $candidate_metadata && ! -L $candidate_metadata ]] || return 1
  sed -n 's/.*"schema_target":\([0-9][0-9]*\).*/\1/p' "$candidate_metadata"
}

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
mapfile -t checksum_fields < <(awk 'NF == 2 {print $1; print $2}' "$checksum")
[[ ${#checksum_fields[@]} -eq 2 && ${checksum_fields[0]} == "$expected_sha" &&
   ${checksum_fields[1]} == "$archive_name" ]] || {
  echo "Backup checksum sidecar is invalid." >&2
  exit 1
}
actual_sha=$(sha256sum "$archive" | awk '{print $1}')
[[ $actual_sha == "$expected_sha" ]] || {
  echo "Backup archive checksum does not match its metadata." >&2
  exit 1
}
zstd -q -t "$archive"
zstd -q -d -o "$raw" "$archive"
actual_sha=$(sha256sum "$raw" | awk '{print $1}')
[[ $actual_sha == "$original_sha" ]] || {
  echo "Decompressed backup checksum does not match its metadata." >&2
  exit 1
}

release_metadata="$release/RELEASE-METADATA.json"
[[ -f $release_metadata && ! -L $release_metadata ]] || {
  echo "Selected release metadata is unavailable." >&2
  exit 1
}
selected_release_id=$(sed -n 's/.*"release_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' "$release_metadata")
selected_deployment_id=$(sed -n \
  's/.*"deployment_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' \
  "$release_metadata")
[[ -n $selected_deployment_id ]] || selected_deployment_id=$selected_release_id
release_schema_value=$(release_schema "$release")
service_unit=$(sed -n 's/.*"service_unit":"\([A-Za-z0-9._+-]*\)".*/\1/p' "$release_metadata")
compatibility=$(sed -n 's/.*"compatibility_release":\(true\|false\).*/\1/p' \
  "$release_metadata")
[[ $release_schema_value == "$schema" &&
   $selected_release_id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
   $selected_deployment_id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
   $(basename "$release") == "$selected_deployment_id" &&
   ($compatibility == true || $compatibility == false) &&
   ($service_unit == sanguinius.service || $service_unit == sanguinius-compat.service) &&
   -f $release/systemd/$service_unit && ! -L $release/systemd/$service_unit ]] || {
  echo "Selected release is not schema-compatible or operationally complete." >&2
  exit 1
}
if [[ $compatibility == false ]]; then
  selected_version=$(sed -n 's/.*"version":"\([0-9.]*\)".*/\1/p' \
    "$release_metadata")
  selected_revision=$(sed -n 's/.*"revision":"\([0-9a-f]*\)".*/\1/p' \
    "$release_metadata")
  selected_catalog=$(sed -n 's/.*"command_catalog_version":\([0-9]*\).*/\1/p' \
    "$release_metadata")
  if ! selected_identity=$("$binary" --version --json); then
    echo "Selected release binary identity is unavailable." >&2
    exit 1
  fi
  [[ $selected_version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ &&
     $selected_revision =~ ^[0-9a-f]{40}$ &&
     $selected_catalog =~ ^[0-9]+$ &&
     $selected_identity == *'"release_id":"'"$selected_release_id"'"'* &&
     $selected_identity == *'"version":"'"$selected_version"'"'* &&
     $selected_identity == *'"revision":"'"$selected_revision"'"'* &&
     $selected_identity == *'"schema_target":'"$schema"* &&
     $selected_identity == *'"command_catalog_version":'"$selected_catalog"* ]] || {
    echo "Selected release binary and metadata disagree." >&2
    exit 1
  }
fi

run_domain_checks "$raw" "$schema" "$binary"
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
exec 9<>"$lock_file"
flock -n 9 || {
  echo "Another Sanguinius operation holds the lock." >&2
  exit 75
}
verify_database_filesystem_entries
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

verify_database_filesystem_entries

current_database_schema=$(sqlite3 -readonly "$database" \
  'SELECT COALESCE(MAX(version),0) FROM schema_migrations;' 2>/dev/null)
[[ $current_database_schema =~ ^[0-9]+$ ]] || {
  echo "The current database schema is unreadable." >&2
  exit 1
}
safety_release=
if [[ -L $current_link ]]; then
  active_release=$(readlink -f -- "$current_link")
  if [[ -d $active_release && ! -L $active_release &&
        $(dirname "$active_release") == "$root/opt/sanguinius/releases" &&
        $(release_schema "$active_release") == "$current_database_schema" ]]; then
    safety_release=$active_release
  fi
fi
if [[ -z $safety_release && $release_schema_value == "$current_database_schema" ]]; then
  safety_release=$release
fi
[[ -n $safety_release && -x $backup_helper ]] || {
  echo "No schema-compatible safety-backup release/helper is available." >&2
  exit 1
}
SANGUINIUS_BACKUP_RELEASE="$safety_release" "$backup_helper" manual --lock-held

stamp=$(date -u +%Y%m%dT%H%M%S%3NZ)
quarantine="$runtime/quarantine-$stamp"
mkdir -m 0700 "$quarantine"
install -m 0600 "$raw" "$ready"
if [[ $test_mode != true ]]; then
  chown sanguinius:sanguinius "$ready"
fi
sync -f "$ready"
verify_database_filesystem_entries
for suffix in '' -wal -shm -journal; do
  if [[ -e $database$suffix || -L $database$suffix ]]; then
    [[ -f $database$suffix && ! -L $database$suffix ]] || {
      echo "A production database sidecar is unsafe." >&2
      exit 1
    }
    sha256sum "$database$suffix" | awk '{print $1}' \
      >"$stage/original${suffix:-database}"
  fi
done
database_switched=true
for suffix in '' -wal -shm -journal; do
  if [[ -e $stage/original${suffix:-database} ]]; then
    mv -T "$database$suffix" "$quarantine/sanguinius.sqlite3$suffix"
    if [[ $suffix == '' && $test_mode == true &&
          ${SANGUINIUS_TEST_FAIL_RESTORE_QUARANTINE:-false} == true ]]; then
      echo "Injected restore quarantine failure." >&2
      exit 1
    fi
  fi
done
mv -T "$ready" "$database"
sync -f "$database"
SANGUINIUS_DATABASE_FILE="$database" "$binary" db migrate
SANGUINIUS_DATABASE_FILE="$database" "$binary" db check
run_domain_checks "$database" "$schema" "$binary"

[[ -L $current_link ]] || {
  echo "The active release link is missing or unsafe." >&2
  exit 1
}
old_current=$(readlink -- "$current_link")
[[ $old_current =~ ^releases/[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
   -d $root/opt/sanguinius/$old_current &&
   ! -L $root/opt/sanguinius/$old_current ]] || {
  echo "The active release link target is outside the managed layout." >&2
  exit 1
}
if [[ -L $previous_link ]]; then
  had_previous=true
  old_previous=$(readlink -- "$previous_link")
  [[ $old_previous =~ ^releases/[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
     -d $root/opt/sanguinius/$old_previous &&
     ! -L $root/opt/sanguinius/$old_previous ]] || {
    echo "The previous release link target is outside the managed layout." >&2
    exit 1
  }
elif [[ -e $previous_link ]]; then
  echo "The previous release path is unsafe." >&2
  exit 1
fi
if [[ -f $state_version && ! -L $state_version ]]; then
  had_state_version=true
  install -m 0600 "$state_version" "$stage/original-state-version"
elif [[ -e $state_version || -L $state_version ]]; then
  echo "The state-version path is unsafe." >&2
  exit 1
fi
if [[ -f $unit_path && ! -L $unit_path ]]; then
  had_unit=true
  install -m 0644 "$unit_path" "$stage/original.service"
elif [[ -e $unit_path || -L $unit_path ]]; then
  echo "The service unit path is unsafe." >&2
  exit 1
fi
activation_started=true
if [[ $old_current != "releases/$selected_deployment_id" ]]; then
  atomic_link "$old_current" "$previous_link"
fi
atomic_link "releases/$selected_deployment_id" "$current_link"
unit_temporary="$(dirname "$unit_path")/.sanguinius.service.ready.$$"
[[ ! -e $unit_temporary && ! -L $unit_temporary ]] || {
  echo "The service-unit staging path already exists." >&2
  exit 1
}
install -m 0644 "$release/systemd/$service_unit" "$unit_temporary"
mv -Tf -- "$unit_temporary" "$unit_path"
unit_temporary=
printf '%s\n' "$schema" >"$stage/state-version.ready"
chmod 0600 "$stage/state-version.ready"
[[ $test_mode == true ]] || chown root:root "$stage/state-version.ready"
mv -Tf -- "$stage/state-version.ready" "$state_version"
if [[ $test_mode != true ]]; then
  systemctl daemon-reload
fi
if [[ $test_mode == true &&
      ${SANGUINIUS_TEST_FAIL_RESTORE_ACTIVATION:-false} == true ]]; then
  echo "Injected restore activation failure." >&2
  exit 1
fi
activation_started=false
database_switched=false
echo "restore=applied"
echo "schema=$schema"
echo "release=$selected_release_id"
echo "deployment=$selected_deployment_id"
echo "quarantine=$(basename "$quarantine")"
