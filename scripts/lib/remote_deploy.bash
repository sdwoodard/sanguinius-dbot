#!/usr/bin/env bash
set -euo pipefail
umask 0077

die() { echo "Deployment refused: $*" >&2; exit 1; }

deployment_systemd_directory=/etc/systemd/system
deployment_configuration_root=

atomic_link() {
  local target=$1 link=$2 temporary
  temporary="$link.new.$$"
  ln -s "$target" "$temporary"
  mv -Tf "$temporary" "$link"
}

remove_managed_tree() {
  local path=$1 parent=$2 prefix=$3
  [[ -n $path && $path == "$parent/$prefix"* && -d $path && ! -L $path ]] ||
    return 1
  find -P "$path" -xdev -depth -delete
}

verify_database_filesystem_entries() {
  local database_path=$1 candidate suffix links
  [[ -f $database_path && ! -L $database_path ]] ||
    die "production database path is unsafe"
  links=$(stat -c %h -- "$database_path")
  [[ $links == 1 ]] || die "production database has a hard-link alias"
  for suffix in .lock -wal -shm -journal; do
    candidate="$database_path$suffix"
    if [[ -e $candidate || -L $candidate ]]; then
      [[ -f $candidate && ! -L $candidate ]] ||
        die "production database companion is unsafe"
      links=$(stat -c %h -- "$candidate")
      [[ $links == 1 ]] ||
        die "production database companion has a hard-link alias"
    fi
  done
}

adopt_database_service_ownership() {
  local database_path=$1 owner=$2 group=$3 candidate suffix owner_id group_id
  owner_id=$(id -u "$owner" 2>/dev/null) ||
    die "database service account is unavailable"
  group_id=$(getent group "$group" | awk -F: 'NR == 1 {print $3}') ||
    die "database service group is unavailable"
  [[ $owner_id =~ ^[0-9]+$ && $group_id =~ ^[0-9]+$ ]] ||
    die "database service identity is invalid"

  verify_database_filesystem_entries "$database_path"
  for suffix in '' .lock -wal -shm -journal; do
    candidate="$database_path$suffix"
    if [[ -e $candidate || -L $candidate ]]; then
      [[ -f $candidate && ! -L $candidate ]] ||
        die "database ownership candidate is unsafe"
      chown -- "$owner:$group" "$candidate" ||
        die "database ownership transfer failed"
      chmod 0600 -- "$candidate" ||
        die "database permission transfer failed"
      [[ $(stat -c '%u:%g:%a' -- "$candidate") == \
         "$owner_id:$group_id:600" ]] ||
        die "database ownership transfer could not be verified"
    fi
  done
}

open_operations_lock() {
  local directory=$1 path=$2
  [[ -d $directory && ! -L $directory ]] ||
    die "operations directory is unsafe"
  if [[ ${SANGUINIUS_SCRIPT_TESTING:-false} != true ]]; then
    [[ $(stat -c '%U:%G:%a' "$directory") == root:sanguinius:750 ]] ||
      die "operations directory ownership or permissions are unsafe"
  fi
  if [[ ! -e $path ]]; then
    install -m 0600 /dev/null "$path"
    if [[ ${SANGUINIUS_SCRIPT_TESTING:-false} != true ]]; then
      chown root:root "$path"
    fi
  fi
  [[ -f $path && ! -L $path ]] || die "operations lock is unsafe"
  if [[ ${SANGUINIUS_SCRIPT_TESTING:-false} != true ]]; then
    [[ $(stat -c '%U:%G:%a' "$path") == root:root:600 ]] ||
      die "operations lock ownership or permissions are unsafe"
  fi
  exec 9<>"$path"
  flock -n 9 || {
    echo "Another deployment or backup is active." >&2
    exit 75
  }
}

write_state_version() {
  local state_directory=$1 runtime_directory=$2 schema=$3
  [[ $schema =~ ^[0-9]+$ && -d $state_directory && ! -L $state_directory &&
     -d $runtime_directory && ! -L $runtime_directory ]] ||
    die "state-version destination is unsafe"
  local temporary="$runtime_directory/state-version.$$"
  [[ ! -e $temporary && ! -L $temporary ]] ||
    die "state-version staging path exists"
  printf '%s\n' "$schema" >"$temporary"
  chmod 0644 "$temporary"
  if [[ ${SANGUINIUS_SCRIPT_TESTING:-false} != true ]]; then
    chown root:root "$temporary"
  fi
  mv -T "$temporary" "$state_directory/state-version"
}

run_schema_domain_checks() {
  local binary=$1 database_file=$2 schema=$3
  SANGUINIUS_DATABASE_FILE="$database_file" "$binary" db integrity
  if (( schema >= 16 )); then
    SANGUINIUS_DATABASE_FILE="$database_file" "$binary" db invariants check
  else
    SANGUINIUS_DATABASE_FILE="$database_file" "$binary" db relationships check
    SANGUINIUS_DATABASE_FILE="$database_file" "$binary" db tarot check
  fi
}

publish_backup_sidecars() {
  local archive=$1 raw=$2 schema=$3 revision=$4 release_id=$5 deployment_id=$6
  local backup_class=$7
  [[ -f $archive && ! -L $archive && -f $raw && ! -L $raw &&
     $schema =~ ^[0-9]+$ && $revision =~ ^[0-9a-f]{40}$ &&
     $release_id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
     $deployment_id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
     $backup_class =~ ^[a-z][a-z-]*$ ]] || die "backup metadata input is invalid"
  local metadata=${archive%.zst}.json checksum=${archive%.zst}.sha256
  [[ ! -e $metadata && ! -L $metadata &&
     ! -e $checksum && ! -L $checksum ]] ||
    die "backup metadata destination exists"
  local original_sha compressed_sha original_size compressed_size created_at_ms
  original_sha=$(sha256sum "$raw" | awk '{print $1}')
  compressed_sha=$(sha256sum "$archive" | awk '{print $1}')
  original_size=$(stat -c %s "$raw")
  compressed_size=$(stat -c %s "$archive")
  created_at_ms=$(date -u +%s%3N)
  printf '%s  %s\n' "$compressed_sha" "$(basename "$archive")" >"$checksum"
  printf '{"format_version":1,"archive":"%s","class":"%s","created_at_ms":%s,"schema":%s,"revision":"%s","release_id":"%s","deployment_id":"%s","original_sha256":"%s","compressed_sha256":"%s","original_size":%s,"compressed_size":%s,"integrity":"ok","foreign_keys":"ok","domain_invariants":"ok","restore_copy":"ok"}\n' \
    "$(basename "$archive")" "$backup_class" "$created_at_ms" "$schema" \
    "$revision" "$release_id" "$deployment_id" "$original_sha" \
    "$compressed_sha" "$original_size" "$compressed_size" >"$metadata"
  chmod 0600 "$metadata" "$checksum"
}

managed_release_name() {
  [[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ ]]
}

metadata_deployment_id() {
  local metadata=$1 release_id deployment_id
  release_id=$(sed -n 's/.*"release_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' \
    "$metadata")
  deployment_id=$(sed -n \
    's/.*"deployment_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' "$metadata")
  [[ -n $deployment_id ]] || deployment_id=$release_id
  printf '%s\n' "$deployment_id"
}

managed_payload_path() {
  case "$1" in
    bin/sanguinius|lib/libdpp.so.10.1.7|RELEASE-METADATA.json|SHARE-MANIFEST.sha256|\
      config/persona.txt|config/appearance-policy-v2.json|\
      config/emperor-tarot-v1.json|config/tarot-house-v1.json|\
      assets/vox/entrance.pcm|assets/vox/error.pcm|assets/vox/farewell.pcm|\
      assets/vox/fallbacks-v1.json|\
      libexec/sanguinius-backup.bash|libexec/sanguinius-restore.bash|\
      systemd/sanguinius.service|systemd/sanguinius-compat.service|\
      systemd/sanguinius-backup.service|systemd/sanguinius-backup.timer|\
      sysusers.d/sanguinius.conf|tmpfiles.d/sanguinius.conf|\
      docs/OPERATIONS.md) return 0 ;;
    migrations/[0-9][0-9][0-9][0-9]_*.sql)
      [[ ${1#migrations/} != */* ]]
      return ;;
    *) return 1 ;;
  esac
}

managed_payload_directory() {
  case "$1" in
    ''|bin|lib|config|assets|assets/vox|migrations|libexec|systemd|sysusers.d|\
      tmpfiles.d|docs) return 0 ;;
    *) return 1 ;;
  esac
}

verify_payload_tree() {
  local release=$1 manifest="$1/SHARE-MANIFEST.sha256"
  [[ -f $manifest && ! -L $manifest ]] || die "payload manifest is missing"
  [[ -z $(find "$release" -mindepth 1 ! -type f ! -type d -print -quit) ]] ||
    die "payload contains an unsupported filesystem entry"
  local hash entry extra relative count=0
  declare -A listed=()
  while read -r hash entry extra; do
    [[ -z $extra && $hash =~ ^[0-9a-f]{64}$ && $entry == ./* ]] ||
      die "payload manifest entry is invalid"
    relative=${entry#./}
    [[ $relative != SHARE-MANIFEST.sha256 ]] ||
      die "payload manifest is self-referential"
    managed_payload_path "$relative" || die "payload manifest path is unmanaged"
    [[ -z ${listed[$relative]+present} ]] || die "payload manifest entry is duplicated"
    listed[$relative]=1
    ((count += 1))
  done <"$manifest"
  (( count > 0 )) || die "payload manifest is empty"
  while IFS= read -r relative; do
    managed_payload_path "$relative" || die "payload file is unmanaged"
    [[ $relative == SHARE-MANIFEST.sha256 ||
       -n ${listed[$relative]+present} ]] || die "payload file is unlisted"
  done < <(find "$release" -type f -printf '%P\n')
  while IFS= read -r relative; do
    managed_payload_directory "$relative" || die "payload directory is unmanaged"
  done < <(find "$release" -type d -printf '%P\n')
  (cd "$release" && sha256sum --quiet -c SHARE-MANIFEST.sha256) ||
    die "payload manifest mismatch"
}

install_system_configuration_files() {
  local release=$1 configuration_root=${2:-} etc_directory=/etc
  if [[ -n $configuration_root ]]; then
    [[ $configuration_root == /* && -d $configuration_root &&
       ! -L $configuration_root ]] || die "system configuration root is unsafe"
    etc_directory="$configuration_root/etc"
  fi
  [[ -f $release/sysusers.d/sanguinius.conf &&
     ! -L $release/sysusers.d/sanguinius.conf &&
     -f $release/tmpfiles.d/sanguinius.conf &&
     ! -L $release/tmpfiles.d/sanguinius.conf ]] ||
    die "system configuration payload is incomplete"
  install -d -m 0755 "$etc_directory/sysusers.d" "$etc_directory/tmpfiles.d"
  [[ -d $etc_directory/sysusers.d && ! -L $etc_directory/sysusers.d &&
     -d $etc_directory/tmpfiles.d && ! -L $etc_directory/tmpfiles.d ]] ||
    die "system configuration directories are unsafe"
  local destination
  for destination in "$etc_directory/sysusers.d/sanguinius.conf" \
    "$etc_directory/tmpfiles.d/sanguinius.conf"; do
    [[ (! -e $destination && ! -L $destination) ||
       (-f $destination && ! -L $destination) ]] ||
      die "system configuration destination is unsafe"
  done
  install -m 0644 "$release/sysusers.d/sanguinius.conf" \
    "$etc_directory/sysusers.d/sanguinius.conf"
  install -m 0644 "$release/tmpfiles.d/sanguinius.conf" \
    "$etc_directory/tmpfiles.d/sanguinius.conf"
}

snapshot_system_configuration_files() {
  local snapshot=$1 configuration_root=${2:-} etc_directory=/etc name source
  if [[ -n $configuration_root ]]; then
    [[ $configuration_root == /* && -d $configuration_root &&
       ! -L $configuration_root ]] || return 1
    etc_directory="$configuration_root/etc"
  fi
  [[ ! -e $snapshot && ! -L $snapshot ]] || return 1
  install -d -m 0700 "$snapshot" || return 1
  for name in sysusers tmpfiles; do
    source="$etc_directory/$name.d/sanguinius.conf"
    if [[ -e $source || -L $source ]]; then
      [[ -f $source && ! -L $source ]] || return 1
      install -m 0600 "$source" "$snapshot/$name.conf" || return 1
      install -m 0600 /dev/null "$snapshot/$name.present" || return 1
    fi
  done
}

restore_system_configuration_files() {
  local snapshot=$1 configuration_root=${2:-} etc_directory=/etc name target
  local rollback_failed=false
  if [[ -n $configuration_root ]]; then
    [[ $configuration_root == /* && -d $configuration_root &&
       ! -L $configuration_root ]] || return 1
    etc_directory="$configuration_root/etc"
  fi
  [[ -d $snapshot && ! -L $snapshot ]] || return 1
  install -d -m 0755 "$etc_directory/sysusers.d" \
    "$etc_directory/tmpfiles.d" || return 1
  [[ -d $etc_directory/sysusers.d && ! -L $etc_directory/sysusers.d &&
     -d $etc_directory/tmpfiles.d && ! -L $etc_directory/tmpfiles.d ]] ||
    return 1
  for name in sysusers tmpfiles; do
    target="$etc_directory/$name.d/sanguinius.conf"
    [[ (! -e $target && ! -L $target) ||
       (-f $target && ! -L $target) ]] || {
      rollback_failed=true
      continue
    }
    if [[ -f $snapshot/$name.present && ! -L $snapshot/$name.present &&
          -f $snapshot/$name.conf && ! -L $snapshot/$name.conf ]]; then
      install -m 0644 "$snapshot/$name.conf" "$target" || {
        rollback_failed=true
        continue
      }
      if [[ $(stat -c %a "$target") != 644 ]] ||
         ! cmp -s "$snapshot/$name.conf" "$target"; then
        rollback_failed=true
      fi
    elif [[ ! -e $snapshot/$name.present &&
            ! -L $snapshot/$name.present &&
            ! -e $snapshot/$name.conf && ! -L $snapshot/$name.conf ]]; then
      rm -f -- "$target" || rollback_failed=true
      [[ ! -e $target && ! -L $target ]] || rollback_failed=true
    else
      rollback_failed=true
    fi
  done
  [[ $rollback_failed == false ]]
}

verify_production_environment() {
  local environment_file=$1
  [[ -f $environment_file && ! -L $environment_file ]] ||
    die "production environment is unsafe"
  ! grep -Eq '^[[:space:]]*(SANGUINIUS_TOKEN|OPENAI_API_KEY)[[:space:]]*=' \
    "$environment_file" ||
    die "environment file contains a credential value"
  local setting key
  for setting in \
    SANGUINIUS_ADMIN_COMMANDS_ENABLED=false \
    SANGUINIUS_TEST_MODE=false \
    SANGUINIUS_VOICE_INPUT_ENABLED=false \
    SANGUINIUS_VOICE_INPUT_GUILD_CONSENT_ATTESTED=false \
    SANGUINIUS_TRANSCRIPTION_PROVIDER=disabled \
    SANGUINIUS_CHRONICLE_ENABLED=true \
    SANGUINIUS_TAROT_ENABLED=true \
    SANGUINIUS_TAROT_HOUSE_ENABLED=true \
    SANGUINIUS_TAROT_INTEGRATION_ENABLED=true \
    SANGUINIUS_VOX_ENABLED=true \
    SANGUINIUS_VOX_NARRATION_ENABLED=true \
    SANGUINIUS_TTS_PROVIDER=openai \
    SANGUINIUS_APPEARANCES_MODE=dry_run \
    SANGUINIUS_DATABASE_FILE=/var/lib/sanguinius/sanguinius.sqlite3 \
    SANGUINIUS_LOG_FILE=/var/log/sanguinius/messages.log \
    SANGUINIUS_OPERATIONS_STATUS_FILE=/var/lib/sanguinius/runtime/operations-status.json \
    SANGUINIUS_BACKUP_DIRECTORY=/var/backups/sanguinius \
    SANGUINIUS_TTS_CACHE_DIRECTORY=/var/cache/sanguinius/tts \
    SANGUINIUS_TTS_CACHE_MAXIMUM_MIB=64 \
    SANGUINIUS_TTS_CACHE_MAXIMUM_DAYS=14 \
    SANGUINIUS_PERSONA_FILE=/opt/sanguinius/current/config/persona.txt \
    SANGUINIUS_APPEARANCE_POLICY_FILE=/opt/sanguinius/current/config/appearance-policy-v2.json \
    SANGUINIUS_TAROT_DECK_FILE=/opt/sanguinius/current/config/emperor-tarot-v1.json \
    SANGUINIUS_TAROT_HOUSE_FILE=/opt/sanguinius/current/config/tarot-house-v1.json \
    SANGUINIUS_TTS_FALLBACK_DIRECTORY=/opt/sanguinius/current/assets/vox \
    SANGUINIUS_FFMPEG_PATH=/usr/bin/ffmpeg \
    SANGUINIUS_FFPROBE_PATH=/usr/bin/ffprobe; do
    key=${setting%%=*}
    [[ $(grep -Ec "^[[:space:]]*${key}[[:space:]]*=" \
      "$environment_file") == 1 ]] ||
      die "production environment setting is missing or duplicated"
    grep -Fqx "$setting" "$environment_file" ||
      die "production environment setting has an unsafe value"
  done
}

retention_deletions() {
  local current_id=$1 previous_id=$2 candidate kept_inactive=1
  shift 2
  for candidate in "$@"; do
    managed_release_name "$candidate" || continue
    [[ $candidate != "$current_id" && $candidate != "$previous_id" ]] ||
      continue
    ((kept_inactive += 1))
    if (( kept_inactive > 3 )); then
      printf '%s\n' "$candidate"
    fi
  done
}

managed_release_directory() {
  local path=$1 expected_id=$2 metadata="$1/RELEASE-METADATA.json"
  managed_release_name "$expected_id" &&
    [[ -d $path && ! -L $path && -f $metadata && ! -L $metadata ]] &&
    [[ $(metadata_deployment_id "$metadata") == "$expected_id" ]]
}

release_retention_deletions() {
  local release_directory=$1 current_id=$2 previous_id=$3 candidate
  local -a recognized=()
  [[ -d $release_directory && ! -L $release_directory ]] || return 1
  while IFS= read -r candidate; do
    managed_release_directory "$release_directory/$candidate" "$candidate" ||
      continue
    recognized+=("$candidate")
  done < <(
    find "$release_directory" -mindepth 1 -maxdepth 1 -type d \
      ! -name '.incoming-*' -printf '%f\n' | LC_ALL=C sort -r
  )
  retention_deletions "$current_id" "$previous_id" "${recognized[@]}"
}

prune_recognized_releases() {
  local release_directory=$1 current_id=$2 previous_id=$3
  shift 3
  local candidate candidate_path candidate_metadata_id protected
  local delete_failed=false skip=false
  local -a expired=()
  mapfile -t expired < <(
    release_retention_deletions "$release_directory" "$current_id" \
      "$previous_id"
  ) || return 1
  for candidate in "${expired[@]}"; do
    skip=false
    for protected in "$@"; do
      if [[ -n $protected && $candidate == "$protected" ]]; then
        skip=true
        break
      fi
    done
    [[ $skip != true ]] || continue
    candidate_path="$release_directory/$candidate"
    [[ -d $candidate_path && ! -L $candidate_path ]] || continue
    [[ -f $candidate_path/RELEASE-METADATA.json ]] || continue
    candidate_metadata_id=$(metadata_deployment_id \
      "$candidate_path/RELEASE-METADATA.json")
    [[ $candidate_metadata_id == "$candidate" ]] || continue
    find "$candidate_path" -depth -type f -delete || delete_failed=true
    find "$candidate_path" -depth -type d -empty -delete || delete_failed=true
  done
  [[ $delete_failed == false ]]
}

failure_action() {
  local expected=$1 target=$2 main_started=$3
  if [[ $expected == "$target" ]]; then
    printf 'rollback-release\n'
  elif [[ $main_started == true ]]; then
    printf 'preserve-schema\n'
  else
    printf 'restore-database\n'
  fi
}

restart_if_previously_active() {
  local prior_state=$1
  [[ $prior_state != active ]] || start_sanguinius_service_verified
}

unit_is_enabled() {
  local state
  state=$(systemctl is-enabled "$1" 2>/dev/null || true)
  [[ $state == enabled || $state == enabled-runtime ]]
}

unit_is_active() {
  [[ $(systemctl is-active "$1" 2>/dev/null || true) == active ]]
}

candidate_service_health_verified() {
  local expected_restarts=$1 active status main_pid restarts processes
  [[ $expected_restarts =~ ^[0-9]+$ ]] || return 1
  active=$(systemctl show -p ActiveState --value sanguinius.service \
    2>/dev/null || true)
  status=$(systemctl show -p StatusText --value sanguinius.service \
    2>/dev/null || true)
  main_pid=$(systemctl show -p MainPID --value sanguinius.service \
    2>/dev/null || true)
  restarts=$(systemctl show -p NRestarts --value sanguinius.service \
    2>/dev/null || true)
  processes=$(pgrep -x sanguinius 2>/dev/null || true)
  [[ $active == active &&
     $status == 'Ready; Discord connected and commands synchronized' &&
     $main_pid =~ ^[1-9][0-9]*$ && $processes == "$main_pid" &&
     $restarts == "$expected_restarts" ]]
}

start_sanguinius_service_verified() {
  systemctl start sanguinius.service && unit_is_active sanguinius.service
}

stop_sanguinius_service_verified() {
  local command_failed=false active_state processes
  systemctl stop sanguinius.service || command_failed=true
  active_state=$(systemctl is-active sanguinius.service 2>/dev/null || true)
  processes=$(pgrep -x sanguinius 2>/dev/null || true)
  [[ $command_failed == false &&
     ($active_state == inactive || $active_state == failed) &&
     -z $processes ]]
}

disable_sanguinius_service_verified() {
  local command_failed=false enabled_state
  systemctl disable sanguinius.service || command_failed=true
  enabled_state=$(systemctl is-enabled sanguinius.service 2>/dev/null || true)
  [[ $command_failed == false && $enabled_state == disabled ]]
}

fence_sanguinius_service_state() {
  local fence_failed=false
  stop_sanguinius_service_verified || fence_failed=true
  disable_sanguinius_service_verified || fence_failed=true
  [[ $fence_failed == false ]]
}

restore_optional_release_link() {
  local present=$1 target=$2 link=$3
  if [[ $present == true ]]; then
    [[ $target =~ ^releases/[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ ]] || return 1
    atomic_link "$target" "$link"
  elif [[ -L $link ]]; then
    unlink -- "$link"
  else
    [[ ! -e $link ]]
  fi
}

safe_counts() {
  sqlite3 -readonly "$1" \
    'SELECT (SELECT count(*) FROM discord_user)||char(58)||(SELECT count(*) FROM chronicle_entry)||char(58)||(SELECT count(*) FROM relationship_event)||char(58)||(SELECT count(*) FROM tarot_transaction)||char(58)||(SELECT count(*) FROM tarot_posting)||char(58)||(SELECT count(*) FROM tarot_wager)||char(58)||(SELECT count(*) FROM voice_session)||char(58)||(SELECT count(*) FROM speech_item);'
}

atomic_restore_database() (
  local archive_path=$1 selected_binary=$2 restore_schema=$3
  local database_path=$4 runtime_directory=$5 failure_label=$6
  [[ -f $archive_path && ! -L $archive_path && -x $selected_binary &&
     $restore_schema =~ ^[0-9]+$ && $database_path == /* &&
     -f $database_path && ! -L $database_path &&
     -d $runtime_directory && ! -L $runtime_directory &&
     $failure_label =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ ]] ||
    die "automatic database restore input is unsafe"
  local stage quarantine decoded ready status current_schema target_schema
  local switch_started=false completed=false suffix exit_code
  stage=$(mktemp -d "$runtime_directory/restore-$failure_label.XXXXXXXX")
  quarantine=
  decoded="$stage/restored.sqlite3"
  ready="$stage/database.ready"
  # Invoked indirectly by the EXIT trap.
  # shellcheck disable=SC2329
  cleanup_atomic_restore() {
    exit_code=$?
    set +e
    if [[ $completed != true && $switch_started == true &&
          -n $quarantine && -d $quarantine && ! -L $quarantine ]]; then
      for suffix in '' -wal -shm -journal; do
        if [[ -e $quarantine/sanguinius.sqlite3$suffix ]]; then
          rm -f -- "$database_path$suffix"
          mv -T "$quarantine/sanguinius.sqlite3$suffix" \
            "$database_path$suffix"
        fi
      done
    fi
    if [[ -d $stage && ! -L $stage ]]; then
      find -P "$stage" -xdev -depth -delete
    fi
    if [[ $completed != true && -n $quarantine && -d $quarantine &&
          ! -L $quarantine ]]; then
      rmdir -- "$quarantine" 2>/dev/null || true
    fi
    exit "$exit_code"
  }
  trap cleanup_atomic_restore EXIT

  zstd -q -t "$archive_path"
  zstd -q -d -o "$decoded" "$archive_path"
  SANGUINIUS_DATABASE_FILE="$decoded" "$selected_binary" db migrate
  SANGUINIUS_DATABASE_FILE="$decoded" "$selected_binary" db check
  run_schema_domain_checks "$selected_binary" "$decoded" "$restore_schema"
  status=$(SANGUINIUS_DATABASE_FILE="$decoded" "$selected_binary" db status)
  current_schema=$(sed -n 's/^current_schema=\([0-9][0-9]*\)$/\1/p' \
    <<<"$status")
  target_schema=$(sed -n 's/^target_schema=\([0-9][0-9]*\)$/\1/p' \
    <<<"$status")
  [[ $current_schema == "$restore_schema" &&
     $target_schema == "$restore_schema" ]] ||
    die "automatic restore release has the wrong schema target"
  SANGUINIUS_DATABASE_FILE="$decoded" "$selected_binary" db backup "$ready"
  SANGUINIUS_DATABASE_FILE="$ready" "$selected_binary" db migrate
  SANGUINIUS_DATABASE_FILE="$ready" "$selected_binary" db check
  run_schema_domain_checks "$selected_binary" "$ready" "$restore_schema"
  chmod 0600 "$ready"
  if [[ ${SANGUINIUS_SCRIPT_TESTING:-false} != true ]]; then
    chown sanguinius:sanguinius "$ready"
  fi
  sync -f "$ready"

  quarantine=$(mktemp -d "$runtime_directory/failed-$failure_label.XXXXXXXX")
  chmod 0700 "$quarantine"
  switch_started=true
  verify_database_filesystem_entries "$database_path"
  for suffix in '' -wal -shm -journal; do
    if [[ -e $database_path$suffix || -L $database_path$suffix ]]; then
      [[ -f $database_path$suffix && ! -L $database_path$suffix ]] ||
        die "production database sidecar is unsafe"
      mv -T "$database_path$suffix" \
        "$quarantine/sanguinius.sqlite3$suffix"
    fi
  done
  if [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true &&
        ${SANGUINIUS_TEST_FAIL_ATOMIC_RESTORE_SWITCH:-false} == true ]]; then
    die "injected automatic restore switch failure"
  fi
  mv -T "$ready" "$database_path"
  sync -f "$database_path"
  completed=true
)

install_service_unit() {
  local release=$1 systemd_directory=${2:-/etc/systemd/system} unit
  [[ -d $systemd_directory && ! -L $systemd_directory ]] ||
    die "systemd unit directory is unsafe"
  unit=$(sed -n 's/.*"service_unit":"\([A-Za-z0-9.-]*\)".*/\1/p' \
    "$release/RELEASE-METADATA.json")
  [[ $unit == sanguinius.service || $unit == sanguinius-compat.service ]] ||
    die "release service unit is invalid"
  [[ -f $release/systemd/$unit && ! -L $release/systemd/$unit ]] ||
    die "release service unit is unavailable"
  install -m 0644 "$release/systemd/$unit" \
    "$systemd_directory/sanguinius.service"
}

install_release_units() {
  local release=$1 systemd_directory=${2:-/etc/systemd/system} unit
  local install_failed=false
  install_service_unit "$release" "$systemd_directory" || install_failed=true
  for unit in sanguinius-backup.service sanguinius-backup.timer; do
    [[ -f $release/systemd/$unit && ! -L $release/systemd/$unit ]] ||
      die "release backup unit is unavailable"
    if [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true &&
          ${SANGUINIUS_TEST_FAIL_UNIT_INSTALL:-} == "$unit" ]]; then
      install_failed=true
      continue
    fi
    install -m 0644 "$release/systemd/$unit" "$systemd_directory/$unit" ||
      install_failed=true
  done
  [[ $install_failed == false ]]
}

require_distinct_rollback_target() {
  local target_id=$1 current_id=$2
  [[ $target_id != "$current_id" ]] ||
    die "rollback target is already active"
}

database_is_exclusive() {
  local fd target
  for fd in /proc/[0-9]*/fd/[0-9]*; do
    [[ -e $fd ]] || continue
    target=$(readlink -f "$fd" 2>/dev/null || true)
    case "$target" in
      "$database"|"$database-wal"|"$database-shm"|"$database-journal")
        return 1 ;;
    esac
  done
}

assert_database_exclusive() {
  database_is_exclusive || die "database is still open by another process"
}

stop_candidate_for_recovery() {
  stop_sanguinius_service_verified && database_is_exclusive
}

fence_schema_changing_candidate() {
  local fence_failed=false
  fence_sanguinius_service_state || fence_failed=true
  database_is_exclusive || fence_failed=true
  [[ $fence_failed == false ]]
}

recover_predeployment_durable_state() {
  local restore_database=$1 rollback_failed=false
  [[ $restore_database == true || $restore_database == false ]] || return 1
  if ! stop_candidate_for_recovery; then
    retain_deploy_diagnostics=true
    echo "Candidate termination or database exclusivity could not be verified; fencing the service." >&2
    fence_schema_changing_candidate ||
      echo "CRITICAL: Sanguinius service fencing could not be verified." >&2
    systemctl stop sanguinius-backup.timer >/dev/null 2>&1 ||
      echo "CRITICAL: Sanguinius backup timer could not be stopped." >&2
    return 1
  fi
  if [[ $restore_database == true ]]; then
    if atomic_restore_database "$pre" "$old/bin/sanguinius" \
        "$expected_schema" "$database" "$runtime" "$new_id"; then
      production_migration_completed=false
      deploy_status_schema=$expected_schema
    else
      rollback_failed=true
    fi
  fi
  restore_optional_release_link "$had_current_before" \
    "$current_before_target" "$current" || rollback_failed=true
  (install_release_units "$old" "$deployment_systemd_directory") ||
    rollback_failed=true
  restore_system_configuration_files "$system_configuration_snapshot" \
    "$deployment_configuration_root" ||
    rollback_failed=true
  restore_optional_release_link "$had_previous_before" \
    "$previous_before_target" "$previous" || rollback_failed=true
  restore_optional_release_link "$had_operations_before" \
    "$operations_before_target" "$operations" || rollback_failed=true
  systemctl daemon-reload || rollback_failed=true
  if [[ $rollback_failed == false && $service_was_enabled == true ]]; then
    systemctl enable sanguinius.service || rollback_failed=true
  elif [[ $rollback_failed == false ]]; then
    systemctl disable sanguinius.service || rollback_failed=true
  fi
  if [[ $rollback_failed == false && $timer_was_enabled == true ]]; then
    systemctl enable sanguinius-backup.timer || rollback_failed=true
  elif [[ $rollback_failed == false ]]; then
    systemctl disable sanguinius-backup.timer || rollback_failed=true
  fi
  if [[ $rollback_failed == false && $timer_was_active == true ]]; then
    systemctl start sanguinius-backup.timer || rollback_failed=true
  elif [[ $rollback_failed == false ]]; then
    systemctl stop sanguinius-backup.timer || rollback_failed=true
  fi
  if [[ $rollback_failed == false ]]; then
    restart_if_previously_active "$active_state" || rollback_failed=true
  fi
  if [[ $rollback_failed == true ]]; then
    retain_deploy_diagnostics=true
    echo "Recovery could not restore a verified durable state; fencing the service." >&2
    fence_schema_changing_candidate ||
      echo "CRITICAL: Sanguinius service fencing could not be verified." >&2
    systemctl stop sanguinius-backup.timer >/dev/null 2>&1 ||
      echo "CRITICAL: Sanguinius backup timer could not be stopped." >&2
    return 1
  fi
}

if [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true && $EUID -ne 0 ]]; then
  test_operation=${1:-}
  shift || true
  case "$test_operation" in
    policy-archive-entry)
      [[ $# -eq 1 && -n $1 && $1 != /* && $1 != *'..'* ]]
      exit ;;
    policy-capacity)
      [[ $# -eq 2 && $1 =~ ^[0-9]+$ && $2 =~ ^[0-9]+$ &&
         $1 -ge 1048576 && $2 -ge 1024 ]]
      exit ;;
    policy-payload-path)
      [[ $# -eq 1 ]] || exit 2
      managed_payload_path "$1"
      exit ;;
    policy-payload-directory)
      [[ $# -eq 1 ]] || exit 2
      managed_payload_directory "$1"
      exit ;;
    policy-payload-tree)
      [[ $# -eq 1 ]] || exit 2
      verify_payload_tree "$1"
      exit ;;
    policy-retention)
      [[ $# -ge 2 ]] || exit 2
      retention_deletions "$@"
      exit ;;
    policy-release-retention)
      [[ $# -eq 3 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      release_retention_deletions "$@"
      exit ;;
    policy-prune-release-retention)
      [[ $# -ge 3 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      prune_recognized_releases "$@"
      exit ;;
    policy-failure)
      [[ $# -eq 3 ]] || exit 2
      failure_action "$@"
      exit ;;
    policy-restore-active-state)
      [[ $# -eq 1 ]] || exit 2
      restart_if_previously_active "$1"
      exit ;;
    policy-ready-failure)
      [[ $# -eq 2 ]] || exit 2
      if [[ $1 == true && $2 != true ]]; then
        fence_sanguinius_service_state
      fi
      exit ;;
    policy-candidate-health)
      [[ $# -eq 1 ]] || exit 2
      candidate_service_health_verified "$1"
      exit ;;
    policy-atomic-link)
      [[ $# -eq 2 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $2 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      atomic_link "$1" "$2"
      exit ;;
    policy-system-configuration)
      [[ $# -eq 1 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      install_system_configuration_files "$1" "$SANGUINIUS_TEST_ROOT"
      exit ;;
    policy-system-configuration-snapshot)
      [[ $# -eq 1 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      snapshot_system_configuration_files "$1" "$SANGUINIUS_TEST_ROOT"
      exit ;;
    policy-system-configuration-restore)
      [[ $# -eq 1 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      restore_system_configuration_files "$1" "$SANGUINIUS_TEST_ROOT"
      exit ;;
    policy-deployment-durable-recovery)
      [[ $# -eq 4 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* && -d $1 && ! -L $1 &&
         ($2 == true || $2 == false) &&
         ($3 == - || ($3 == "$SANGUINIUS_TEST_ROOT"/* &&
                      -f $3 && ! -L $3)) &&
         $4 =~ ^[0-9]+$ ]] || exit 2
      recovery_test_root=$1
      restore_database=$2
      pre=$3
      expected_schema=$4
      target_schema=$4
      state="$recovery_test_root/var/lib/sanguinius"
      runtime="$state/runtime"
      database="$state/sanguinius.sqlite3"
      releases="$recovery_test_root/opt/sanguinius/releases"
      current="$recovery_test_root/opt/sanguinius/current"
      previous="$recovery_test_root/opt/sanguinius/previous"
      operations="$recovery_test_root/opt/sanguinius/operations"
      old="$releases/old"
      new_id=candidate
      system_configuration_snapshot="$recovery_test_root/system-configuration-before"
      deployment_systemd_directory="$recovery_test_root/etc/systemd/system"
      deployment_configuration_root=$recovery_test_root
      had_current_before=true
      current_before_target=releases/old
      had_previous_before=true
      previous_before_target=releases/prior
      had_operations_before=true
      operations_before_target=releases/operations-old
      service_was_enabled=false
      timer_was_enabled=false
      timer_was_active=false
      active_state=inactive
      production_migration_completed=true
      deploy_status_schema=$4
      [[ -d $runtime && ! -L $runtime && -f $database &&
         ! -L $database && -x $old/bin/sanguinius &&
         -d $system_configuration_snapshot &&
         ! -L $system_configuration_snapshot ]] || exit 2
      recover_predeployment_durable_state "$restore_database"
      exit ;;
    policy-release-units)
      [[ $# -eq 2 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* &&
         $2 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      install_release_units "$@"
      exit ;;
    policy-distinct-rollback)
      [[ $# -eq 2 ]] || exit 2
      require_distinct_rollback_target "$@"
      exit ;;
    policy-production-environment)
      [[ $# -eq 1 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      verify_production_environment "$1"
      exit ;;
    policy-safe-counts)
      [[ $# -eq 1 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* && -f $1 && ! -L $1 ]] || exit 2
      safe_counts "$1"
      exit ;;
    policy-database-filesystem)
      [[ $# -eq 1 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      verify_database_filesystem_entries "$1"
      exit ;;
    policy-database-ownership)
      [[ $# -eq 3 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      adopt_database_service_ownership "$@"
      exit ;;
    policy-backup-sidecars)
      [[ $# -eq 7 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* &&
         $2 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      publish_backup_sidecars "$@"
      exit ;;
    policy-atomic-database-restore)
      [[ $# -eq 6 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT"/* &&
         $2 == "$SANGUINIUS_TEST_ROOT"/* &&
         $4 == "$SANGUINIUS_TEST_ROOT"/* &&
         $5 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      atomic_restore_database "$@"
      exit ;;
    policy-remove-managed-tree)
      [[ $# -eq 1 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $1 == "$SANGUINIUS_TEST_ROOT/runtime/deploy-"* ]] || exit 2
      remove_managed_tree "$1" "$SANGUINIUS_TEST_ROOT/runtime" deploy-
      exit ;;
    *) exit 2 ;;
  esac
fi

operation=${1:-}
shift || true
expected_helper_sha=
archive=
expected_archive_sha=
expected_schema=
target_schema=
environment=
token=
openai_key=
message_log=
release_id=
while (( $# )); do
  case "$1" in
    --expected-helper-sha) expected_helper_sha=$2; shift 2 ;;
    --archive) archive=$2; shift 2 ;;
    --expected-archive-sha) expected_archive_sha=$2; shift 2 ;;
    --expected-schema) expected_schema=$2; shift 2 ;;
    --target-schema) target_schema=$2; shift 2 ;;
    --environment) environment=$2; shift 2 ;;
    --token) token=$2; shift 2 ;;
    --openai-key) openai_key=$2; shift 2 ;;
    --message-log) message_log=$2; shift 2 ;;
    --release-id) release_id=$2; shift 2 ;;
    *) die "unknown argument" ;;
  esac
done

[[ $EUID -eq 0 ]] || die "root is required"
[[ $(hostname -s) == nuln ]] || die "unexpected host"
if [[ -r /proc/$$/fd/255 ]]; then
  helper_path=$(readlink -f /proc/$$/fd/255)
elif [[ -f ${BASH_SOURCE[0]} ]]; then
  helper_path=$(readlink -f "${BASH_SOURCE[0]}")
else
  helper_path=
fi
[[ $expected_helper_sha =~ ^[0-9a-f]{64}$ && -n $helper_path &&
   -f $helper_path && ! -L $helper_path ]] || die "helper identity is unavailable"
[[ $(sha256sum "$helper_path" | awk '{print $1}') == "$expected_helper_sha" ]] ||
  die "helper checksum mismatch"
[[ $(uname -m) == x86_64 ]] || die "unsupported architecture"
for tool in flock sha256sum tar zstd readelf ldd systemctl systemd-analyze sqlite3; do
  command -v "$tool" >/dev/null || die "required tool unavailable: $tool"
done

state=/var/lib/sanguinius
runtime=$state/runtime
database=$state/sanguinius.sqlite3
backups=/var/backups/sanguinius
releases=/opt/sanguinius/releases
current=/opt/sanguinius/current
previous=/opt/sanguinius/previous
operations=/opt/sanguinius/operations
lock_file=$runtime/operations.lock

schema_version() {
  sqlite3 -readonly "$1" \
    'SELECT COALESCE(MAX(version),0) FROM schema_migrations;' 2>/dev/null
}

assert_process_state() {
  local active=$1 main_pid processes
  processes=$(pgrep -x sanguinius 2>/dev/null || true)
  if [[ $active == active ]]; then
    main_pid=$(systemctl show -p MainPID --value sanguinius.service)
    [[ $main_pid =~ ^[1-9][0-9]*$ && $processes == "$main_pid" ]] ||
      die "service process ownership is ambiguous"
  else
    [[ -z $processes ]] || die "an unmanaged Sanguinius process is running"
  fi
}

verify_capacity() {
  local directory available_kib available_inodes
  for directory in "$state" "$releases"; do
    available_kib=$(df -Pk "$directory" | awk 'NR == 2 {print $4}')
    available_inodes=$(df -Pi "$directory" | awk 'NR == 2 {print $4}')
    [[ $available_kib =~ ^[0-9]+$ && $available_inodes =~ ^[0-9]+$ &&
       $available_kib -ge 1048576 && $available_inodes -ge 1024 ]] ||
      die "insufficient deployment filesystem capacity"
  done
}

inject_deploy_setup_failure() {
  local step=$1
  if [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true &&
        ${SANGUINIUS_TEST_FAIL_DEPLOY_SETUP_STEP:-} == "$step" ]]; then
    die "injected deployment setup failure after $step"
  fi
}

verify_secret_source() {
  local path=$1
  [[ $path == /* && -f $path && ! -L $path ]] || die "credential source is unsafe"
  local mode
  mode=$(stat -c %a "$path")
  (( (8#$mode & 0077) == 0 )) || die "credential source permissions are too broad"
}

verify_archive() {
  [[ $archive =~ ^/tmp/sanguinius-upload\.[A-Za-z0-9]{8}/sanguinius-[A-Za-z0-9._+-]+\.tar\.zst$ &&
     -f $archive && ! -L $archive ]] || die "archive path is unsafe"
  [[ $expected_archive_sha =~ ^[0-9a-f]{64}$ ]] || die "archive hash is invalid"
  [[ $(sha256sum "$archive" | awk '{print $1}') == "$expected_archive_sha" ]] ||
    die "archive checksum mismatch"
  local entry root=
  while IFS= read -r entry; do
    [[ -n $entry && $entry != /* && $entry != *'..'* ]] || die "unsafe archive path"
    local first=${entry%%/*}
    [[ -n $root ]] || root=$first
    [[ $root == "$first" ]] || die "archive has multiple roots"
  done < <(tar --zstd -tf "$archive")
  [[ $root =~ ^sanguinius-[A-Za-z0-9._+-]+$ ]] || die "invalid release root"
  ! tar --zstd -tvf "$archive" | awk '$1 !~ /^[-d]/ {found=1} END {exit !found}' ||
    die "archive contains an unsupported entry type"
  printf '%s\n' "$root"
}

verify_compatibility_binary_identity() (
  local release=$1 scratch_root=$2 identity_release_id=$3 version=$4 revision=$5 schema=$6
  local catalog=$7
  [[ -d $release && ! -L $release && -x $release/bin/sanguinius &&
     -d $scratch_root && ! -L $scratch_root &&
     $identity_release_id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
     $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ &&
     $revision =~ ^[0-9a-f]{40}$ && $schema =~ ^[0-9]+$ &&
     $catalog =~ ^[0-9]+$ ]] ||
    die "compatibility identity input is unsafe"
  local scratch summary status target version_json
  scratch=$(mktemp -d "$scratch_root/identity.XXXXXXXX")
  trap 'find -P "$scratch" -xdev -depth -delete' EXIT
  install -d -m 0700 "$scratch/cache" "$scratch/backups"
  if version_json=$("$release/bin/sanguinius" --version --json 2>/dev/null); then
    [[ $version_json == *'"release_id":"'"$identity_release_id"'"'* &&
       $version_json == *'"version":"'"$version"'"'* &&
       $version_json == *'"revision":"'"$revision"'"'* &&
       $version_json == *'"schema_target":'"$schema"* &&
       $version_json == *'"command_catalog_version":'"$catalog"* ]] ||
      die "compatibility binary and metadata disagree"
    return
  fi
  summary=$(
    cd "$release"
    /usr/bin/env -i PATH=/usr/bin:/bin \
      SANGUINIUS_TOKEN=SANGUINIUS_IDENTITY_DISCORD_SENTINEL \
      OPENAI_API_KEY=SANGUINIUS_IDENTITY_OPENAI_SENTINEL \
      SANGUINIUS_GUILD_ID=10 \
      SANGUINIUS_PRIMARY_CHANNEL_ID=20 \
      SANGUINIUS_OWNER_USER_ID=30 \
      SANGUINIUS_DATABASE_FILE="$scratch/identity.sqlite3" \
      SANGUINIUS_LOG_FILE="$scratch/messages.log" \
      SANGUINIUS_OPERATIONS_STATUS_FILE="$scratch/operations-status.json" \
      SANGUINIUS_BACKUP_DIRECTORY="$scratch/backups" \
      SANGUINIUS_TTS_CACHE_DIRECTORY="$scratch/cache" \
      SANGUINIUS_PERSONA_FILE="$release/config/persona.txt" \
      SANGUINIUS_APPEARANCE_POLICY_FILE="$release/config/appearance-policy-v2.json" \
      SANGUINIUS_TAROT_DECK_FILE="$release/config/emperor-tarot-v1.json" \
      SANGUINIUS_TAROT_HOUSE_FILE="$release/config/tarot-house-v1.json" \
      SANGUINIUS_TTS_FALLBACK_DIRECTORY="$release/assets/vox" \
      SANGUINIUS_OPENAI_INPUT_MICRO_USD_PER_MILLION_TOKENS=1 \
      SANGUINIUS_OPENAI_OUTPUT_MICRO_USD_PER_MILLION_TOKENS=1 \
      SANGUINIUS_ADMIN_COMMANDS_ENABLED=false \
      SANGUINIUS_TEST_MODE=false \
      SANGUINIUS_APPEARANCES_MODE=off \
      SANGUINIUS_VOICE_INPUT_ENABLED=false \
      SANGUINIUS_VOICE_INPUT_GUILD_CONSENT_ATTESTED=false \
      SANGUINIUS_TRANSCRIPTION_PROVIDER=disabled \
      "$release/bin/sanguinius" --check-config
  ) || die "compatibility configuration identity failed"
  grep -Fqx "version=$version" <<<"$summary" ||
    die "compatibility version metadata disagrees"
  grep -Fqx "revision=$revision" <<<"$summary" ||
    die "compatibility revision metadata disagrees"
  status=$(SANGUINIUS_DATABASE_FILE="$scratch/absent.sqlite3" \
    "$release/bin/sanguinius" db status) ||
    die "compatibility schema identity failed"
  target=$(sed -n 's/^target_schema=\([0-9][0-9]*\)$/\1/p' <<<"$status")
  [[ $target == "$schema" ]] || die "compatibility schema metadata disagrees"
)

verify_release_binary_identity() {
  local release=$1 scratch_root=${2:-$runtime} metadata="$1/RELEASE-METADATA.json"
  [[ -d $release && ! -L $release && -f $metadata && ! -L $metadata &&
     -x $release/bin/sanguinius ]] || die "release identity input is unsafe"
  local compatibility id deployment_id version revision schema catalog version_json
  compatibility=$(sed -n 's/.*"compatibility_release":\(true\|false\).*/\1/p' \
    "$metadata")
  id=$(sed -n 's/.*"release_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' "$metadata")
  deployment_id=$(metadata_deployment_id "$metadata")
  version=$(sed -n 's/.*"version":"\([0-9.]*\)".*/\1/p' "$metadata")
  revision=$(sed -n 's/.*"revision":"\([0-9a-f]*\)".*/\1/p' "$metadata")
  schema=$(sed -n 's/.*"schema_target":\([0-9]*\).*/\1/p' "$metadata")
  catalog=$(sed -n 's/.*"command_catalog_version":\([0-9]*\).*/\1/p' \
    "$metadata")
  [[ ($compatibility == true || $compatibility == false) &&
     $id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
     $deployment_id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
     $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ &&
     $revision =~ ^[0-9a-f]{40}$ && $schema =~ ^[0-9]+$ &&
     $catalog =~ ^[0-9]+$ ]] || die "release metadata identity is invalid"
  if [[ $compatibility == true ]]; then
    verify_compatibility_binary_identity "$release" "$scratch_root" \
      "$id" "$version" "$revision" "$schema" "$catalog"
    return
  fi
  version_json=$("$release/bin/sanguinius" --version --json) ||
    die "release self-identification failed"
  [[ $version_json == *'"release_id":"'"$id"'"'* &&
     $version_json == *'"version":"'"$version"'"'* &&
     $version_json == *'"revision":"'"$revision"'"'* &&
     $version_json == *'"schema_target":'"$schema"* &&
     $version_json == *'"command_catalog_version":'"$catalog"* ]] ||
    die "binary and release metadata disagree"
}

install_release() {
  local root=$1
  local allow_existing=${2:-false}
  local id=${root#sanguinius-}
  local incoming="$releases/.incoming-$id"
  local installed="$releases/$id"
  if [[ $allow_existing == true && -d $installed && ! -L $installed &&
        ! -e $incoming ]]; then
    verify_payload_tree "$installed"
    verify_release_binary_identity "$installed"
    [[ $(metadata_deployment_id "$installed/RELEASE-METADATA.json") == "$id" ]] ||
      die "existing deployment ID conflicts"
    [[ $(sha256sum "$installed/RELEASE-METADATA.json" | awk '{print $1}') == \
       $(tar --zstd -xOf "$archive" "$root/RELEASE-METADATA.json" | \
         sha256sum | awk '{print $1}') ]] || die "existing release metadata conflicts"
    [[ $(sha256sum "$installed/SHARE-MANIFEST.sha256" | awk '{print $1}') == \
       $(tar --zstd -xOf "$archive" "$root/SHARE-MANIFEST.sha256" | \
         sha256sum | awk '{print $1}') ]] || die "existing release manifest conflicts"
    [[ $(stat -c '%U:%G' "$installed") == root:root &&
       -z $(find "$installed" -xdev \
         \( ! -user root -o ! -group root -o -perm /022 \) -print -quit) ]] ||
      die "existing release ownership or permissions are unsafe"
    printf '%s\n' "$id"
    return
  fi
  [[ $allow_existing == false || $allow_existing == true ]] ||
    die "invalid existing-release policy"
  local incoming_committed=false
  # Invoked indirectly by the RETURN/EXIT paths in the command-substitution
  # subshell used by callers.
  # shellcheck disable=SC2329
  cleanup_incoming() {
    if [[ $incoming_committed != true && -d $incoming && ! -L $incoming &&
          $incoming == "$releases/.incoming-"* ]]; then
      find "$incoming" -xdev -depth -type f -delete
      find "$incoming" -xdev -depth -type l -delete
      find "$incoming" -xdev -depth -type d -empty -delete
    fi
  }
  trap cleanup_incoming EXIT
  [[ ! -e $installed && ! -e $incoming ]] || die "release ID already exists"
  mkdir -m 0700 "$incoming"
  tar --zstd -xf "$archive" -C "$incoming" --strip-components=1 \
    --no-same-owner --no-same-permissions
  [[ -f $incoming/RELEASE-METADATA.json &&
     -f $incoming/SHARE-MANIFEST.sha256 &&
     -x $incoming/bin/sanguinius &&
     -f $incoming/lib/libdpp.so.10.1.7 ]] || die "release payload incomplete"
  verify_payload_tree "$incoming"
  local embedded_deployment_id
  embedded_deployment_id=$(metadata_deployment_id \
    "$incoming/RELEASE-METADATA.json")
  [[ $embedded_deployment_id == "$id" ]] || die "conflicting deployment ID"
  [[ $(readelf -l "$incoming/bin/sanguinius" | \
      sed -n 's/.*interpreter: \([^]]*\).*/\1/p') == /lib64/ld-linux-x86-64.so.2 ]] ||
    die "unexpected ELF interpreter"
  local rpath
  rpath=$(readelf -d "$incoming/bin/sanguinius" | \
    sed -n 's/.*\(RUNPATH\|RPATH\).*\[\(.*\)\].*/\2/p')
  [[ $rpath == "\$ORIGIN/../lib" ]] || die "unexpected RPATH"
  ! LD_LIBRARY_PATH="$incoming/lib" ldd "$incoming/bin/sanguinius" | \
    grep -q 'not found' || die "missing ELF dependency"
  ! readelf -n "$incoming/bin/sanguinius" "$incoming/lib/libdpp.so.10.1.7" | \
    grep -q 'x86-64-v4' || die "release requires x86-64-v4"
  local maximum_glibc maximum_glibcxx
  maximum_glibc=$(readelf --version-info "$incoming/bin/sanguinius" \
    "$incoming/lib/libdpp.so.10.1.7" | \
    sed -n 's/.*Name: GLIBC_\([0-9.]*\).*/\1/p' | sort -V | tail -n1)
  maximum_glibcxx=$(readelf --version-info "$incoming/bin/sanguinius" \
    "$incoming/lib/libdpp.so.10.1.7" | \
    sed -n 's/.*Name: GLIBCXX_\([0-9.]*\).*/\1/p' | sort -V | tail -n1)
  [[ -z $maximum_glibc ||
     $(printf '%s\n%s\n' "$maximum_glibc" 2.44 | sort -V | tail -n1) == 2.44 ]] ||
    die "release requires a newer glibc"
  [[ -z $maximum_glibcxx ||
     $(printf '%s\n%s\n' "$maximum_glibcxx" 3.4.36 | sort -V | tail -n1) == 3.4.36 ]] ||
    die "release requires a newer libstdc++"
  verify_release_binary_identity "$incoming"
  find "$incoming" -type d -exec chmod 0755 '{}' +
  find "$incoming" -type f -exec chmod 0444 '{}' +
  chmod 0555 "$incoming/bin/sanguinius" "$incoming/lib/libdpp.so.10.1.7"
  [[ ! -d $incoming/libexec ]] || chmod 0555 "$incoming/libexec"/*
  chown -R root:root "$incoming"
  mv -T "$incoming" "$installed"
  incoming_committed=true
  trap - EXIT
  printf '%s\n' "$id"
}

write_status() {
  local result=$1 schema=${2:-0}
  local now backup_at=0 backup_schema=0 backup_result=never
  now=$(date -u +%s%3N)
  if [[ -f $runtime/operations-status.json &&
        ! -L $runtime/operations-status.json ]]; then
    backup_at=$(sed -n 's/.*"backup_at_ms":\([0-9]*\).*/\1/p' \
      "$runtime/operations-status.json")
    backup_schema=$(sed -n 's/.*"backup_schema":\([0-9]*\).*/\1/p' \
      "$runtime/operations-status.json")
    backup_result=$(sed -n 's/.*"backup_result":"\([a-z]*\)".*/\1/p' \
      "$runtime/operations-status.json")
  fi
  [[ $backup_at =~ ^[0-9]+$ ]] || backup_at=0
  [[ $backup_schema =~ ^[0-9]+$ ]] || backup_schema=0
  [[ $backup_result =~ ^(never|succeeded|failed)$ ]] || backup_result=never
  if [[ ${3:-} == backup-succeeded ]]; then
    backup_at=$now
    backup_schema=$schema
    backup_result=succeeded
  fi
  local temporary="$runtime/operations-status.json.$$"
  printf '{"version":1,"updated_at_ms":%s,"result":"%s","backup_at_ms":%s,"backup_schema":%s,"backup_result":"%s"}\n' \
    "$now" "$result" "$backup_at" "$backup_schema" "$backup_result" \
    >"$temporary"
  chmod 0644 "$temporary"
  chown root:root "$temporary"
  mv -T "$temporary" "$runtime/operations-status.json"
}

run_candidate_config_check() {
  local release=$1 unit="sanguinius-config-check-$$"
  [[ -d $release && ! -L $release && -x $release/bin/sanguinius ]] ||
    die "candidate configuration check release is unsafe"
  systemd-run --quiet --wait --collect --pipe --service-type=exec \
    --unit "$unit" --uid sanguinius --gid sanguinius \
    --working-directory "$release" \
    --property=NoNewPrivileges=yes --property=PrivateTmp=yes \
    --property=ProtectHome=yes --property=ProtectSystem=strict \
    --property=EnvironmentFile=/etc/sanguinius/sanguinius.env \
    --property=LoadCredential=discord-token:/etc/sanguinius/bot.token \
    --property=LoadCredential=openai-key:/etc/sanguinius/openai.key \
    /usr/bin/env \
    'SANGUINIUS_TOKEN_FILE=%d/discord-token' \
    'SANGUINIUS_OPENAI_API_KEY_FILE=%d/openai-key' \
    "SANGUINIUS_PERSONA_FILE=$release/config/persona.txt" \
    "SANGUINIUS_APPEARANCE_POLICY_FILE=$release/config/appearance-policy-v2.json" \
    "SANGUINIUS_TAROT_DECK_FILE=$release/config/emperor-tarot-v1.json" \
    "SANGUINIUS_TAROT_HOUSE_FILE=$release/config/tarot-house-v1.json" \
    "SANGUINIUS_TTS_FALLBACK_DIRECTORY=$release/assets/vox" \
    "$release/bin/sanguinius" --check-config ||
    die "candidate production configuration is invalid"
}

select_release_for_schema() {
  local release=$1 schema=$2
  [[ $release == "$releases/"* && -d $release && ! -L $release &&
     $schema =~ ^[0-9]+$ ]] || die "rollback release selection is unsafe"
  atomic_link "releases/$(basename "$release")" "$current"
  install_service_unit "$release"
  systemctl daemon-reload
  write_state_version "$state" "$runtime" "$schema"
}

case "$operation" in
bootstrap)
  [[ -n $archive && -n $environment && -n $token && -n $openai_key &&
     -n $message_log ]] || die "bootstrap arguments are incomplete"
  active_state=$(systemctl is-active sanguinius.service 2>/dev/null || true)
  assert_process_state "$active_state"
  [[ $active_state != active ]] ||
    die "service must be inactive"
  verify_database_filesystem_entries "$database"
  [[ $(schema_version "$database") == 13 ]] ||
    die "expected schema-13 database is unavailable"
  verify_secret_source "$environment"
  verify_secret_source "$token"
  verify_secret_source "$openai_key"
  [[ $message_log == /* && -f $message_log && ! -L $message_log ]] ||
    die "message log source is unsafe"
  verify_production_environment "$environment"
  release_root=$(verify_archive)
  if ! getent passwd sanguinius >/dev/null; then
    systemd-sysusers --inline \
      'u sanguinius - "Sanguinius Discord bot" /var/lib/sanguinius /usr/bin/nologin'
  fi
  install -d -m 0700 -o root -g root /etc/sanguinius
  install -d -m 0710 -o root -g sanguinius "$backups"
  install -d -m 0755 -o root -g root /opt/sanguinius "$releases"
  install -d -m 0750 -o root -g sanguinius "$runtime"
  install -d -m 0700 -o sanguinius -g sanguinius \
    /var/cache/sanguinius /var/cache/sanguinius/tts /var/log/sanguinius
  verify_capacity
  open_operations_lock "$runtime" "$lock_file"
  rollback_id=$(install_release "$release_root" true)
  rollback="$releases/$rollback_id"
  [[ $(sed -n 's/.*"schema_target":\([0-9]*\).*/\1/p' \
      "$rollback/RELEASE-METADATA.json") == 13 ]] || die "rollback schema mismatch"
  raw="$runtime/bootstrap-schema13.sqlite3"
  rollback_revision=$(sed -n 's/.*"revision":"\([0-9a-f]*\)".*/\1/p' \
    "$rollback/RELEASE-METADATA.json")
  [[ $rollback_revision =~ ^[0-9a-f]{40}$ ]] || die "rollback revision is invalid"
  compressed="$backups/$(date -u +%Y%m%dT%H%M%SZ)-schema13-${rollback_revision:0:12}-pre-migration.sqlite3.zst"
  [[ ! -e $raw && ! -e $compressed ]] || die "bootstrap backup destination exists"
  SANGUINIUS_DATABASE_FILE="$database" "$rollback/bin/sanguinius" db backup "$raw"
  SANGUINIUS_DATABASE_FILE="$raw" "$rollback/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$raw" "$rollback/bin/sanguinius" db relationships check
  SANGUINIUS_DATABASE_FILE="$raw" "$rollback/bin/sanguinius" db tarot check
  zstd -q -19 -T1 -o "$compressed" "$raw"
  zstd -q -t "$compressed"
  restored="$runtime/bootstrap-restore.sqlite3"
  [[ ! -e $restored ]] || die "bootstrap restore rehearsal already exists"
  zstd -q -d -o "$restored" "$compressed"
  SANGUINIUS_DATABASE_FILE="$restored" "$rollback/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$restored" "$rollback/bin/sanguinius" db relationships check
  SANGUINIUS_DATABASE_FILE="$restored" "$rollback/bin/sanguinius" db tarot check
  SANGUINIUS_DATABASE_FILE="$restored" "$rollback/bin/sanguinius" db migrate
  SANGUINIUS_DATABASE_FILE="$restored" "$rollback/bin/sanguinius" db check
  rollback_release_id=$(sed -n \
    's/.*"release_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' \
    "$rollback/RELEASE-METADATA.json")
  publish_backup_sidecars "$compressed" "$raw" 13 "$rollback_revision" \
    "$rollback_release_id" "$rollback_id" pre-migration
  rm -f -- "$raw" "$restored"
  install -d -m 0750 -o sanguinius -g sanguinius "$state"
  install -d -m 0750 -o root -g sanguinius "$runtime"
  install -m 0600 -o root -g root "$environment" /etc/sanguinius/sanguinius.env
  install -m 0600 -o root -g root "$token" /etc/sanguinius/bot.token
  install -m 0600 -o root -g root "$openai_key" /etc/sanguinius/openai.key
  install -m 0600 -o sanguinius -g sanguinius "$message_log" \
    /var/log/sanguinius/messages.log
  adopt_database_service_ownership "$database" sanguinius sanguinius
  write_state_version "$state" "$runtime" 13
  atomic_link "releases/$rollback_id" "$previous"
  install_service_unit "$rollback"
  install_system_configuration_files "$rollback"
  systemctl daemon-reload
  write_status succeeded 13 backup-succeeded
  echo "bootstrap=complete"
  echo "rollback_release=$rollback_id"
  ;;
deploy)
  [[ $expected_schema =~ ^[0-9]+$ && $target_schema =~ ^[0-9]+$ ]] ||
    die "schema arguments are invalid"
  [[ -d $runtime && -d $releases && ! -L $runtime && ! -L $releases ]] ||
    die "production layout is not bootstrapped"
  verify_database_filesystem_entries "$database"
  [[ $(stat -c '%U:%G:%a' "$runtime") == sanguinius:sanguinius:700 ||
     $(stat -c '%U:%G:%a' "$runtime") == root:sanguinius:750 ]] ||
    die "operations directory cannot be safely upgraded"
  [[ $(stat -c '%U:%G:%a' "$backups") == root:root:700 ||
     $(stat -c '%U:%G:%a' "$backups") == root:sanguinius:710 ]] ||
    die "backup directory cannot be safely upgraded"
  install -d -m 0750 -o root -g sanguinius "$runtime"
  install -d -m 0710 -o root -g sanguinius "$backups"
  verify_secret_source /etc/sanguinius/sanguinius.env
  verify_secret_source /etc/sanguinius/bot.token
  verify_secret_source /etc/sanguinius/openai.key
  for credential_file in /etc/sanguinius/sanguinius.env \
    /etc/sanguinius/bot.token /etc/sanguinius/openai.key; do
    [[ $(stat -c '%U:%G' "$credential_file") == root:root ]] ||
      die "production credential ownership is unsafe"
  done
  verify_production_environment /etc/sanguinius/sanguinius.env
  open_operations_lock "$runtime" "$lock_file"
  while IFS= read -r stale_disposable; do
    [[ $(stat -c '%U:%G' "$stale_disposable") == root:root ]] ||
      die "stale disposable deployment directory is not root-owned"
    [[ ! -e $stale_disposable/retain-for-operator &&
       ! -L $stale_disposable/retain-for-operator ]] ||
      die "retained deployment diagnostics require operator recovery"
    remove_managed_tree "$stale_disposable" "$runtime" deploy-
  done < <(find "$runtime" -mindepth 1 -maxdepth 1 -type d \
    -name 'deploy-*' -print)
  write_status running "$expected_schema"
  deploy_succeeded=false
  retain_deploy_diagnostics=false
  candidate_gateway_ready=false
  candidate_durable_setup_started=false
  candidate_main_started=false
  candidate_failure_handled=false
  deploy_status_schema=$expected_schema
  disposable=
  system_configuration_snapshot=
  old=
  old_unit=
  service_was_enabled=false
  timer_was_enabled=false
  timer_was_active=false
  had_current_before=false
  current_before_target=
  had_previous_before=false
  previous_before_target=
  had_operations_before=false
  operations_before_target=
  service_stopped_for_deploy=false
  database_exclusive_for_deploy=false
  production_migration_completed=false
  unit_is_enabled sanguinius.service && service_was_enabled=true
  unit_is_enabled sanguinius-backup.timer && timer_was_enabled=true
  unit_is_active sanguinius-backup.timer && timer_was_active=true
  # Invoked indirectly by the EXIT trap.
  # shellcheck disable=SC2329
  record_deploy_failure() {
    local exit_code=$?
    set +e
    if [[ $candidate_gateway_ready == true && $deploy_succeeded != true ]]; then
      if [[ $expected_schema == "$target_schema" ]]; then
        recover_predeployment_durable_state false || true
      else
        if ! fence_schema_changing_candidate; then
          retain_deploy_diagnostics=true
          echo "CRITICAL: Schema-changing candidate fencing could not be verified." >&2
        fi
      fi
    elif [[ $candidate_durable_setup_started == true &&
            $candidate_failure_handled != true &&
            $production_migration_completed == true ]]; then
      if [[ $expected_schema == "$target_schema" ]]; then
        recover_predeployment_durable_state false || true
      elif [[ $candidate_main_started == true ]]; then
        if ! fence_schema_changing_candidate; then
          retain_deploy_diagnostics=true
          echo "CRITICAL: Schema-changing candidate fencing could not be verified." >&2
        fi
      else
        recover_predeployment_durable_state true || true
      fi
    elif [[ $candidate_failure_handled != true &&
            $service_stopped_for_deploy == true &&
            $database_exclusive_for_deploy == true &&
            $production_migration_completed != true &&
            $active_state == active ]]; then
      restart_if_previously_active "$active_state" >/dev/null 2>&1 ||
        echo "CRITICAL: Previous Sanguinius service restart could not be verified." >&2
    fi
    if [[ $retain_deploy_diagnostics == true && -n $disposable &&
          -d $disposable && ! -L $disposable ]]; then
      install -m 0600 /dev/null "$disposable/retain-for-operator" || true
      echo "CRITICAL: Privileged deployment diagnostics were retained for operator recovery." >&2
    elif [[ -n $disposable && -d $disposable && ! -L $disposable ]]; then
      remove_managed_tree "$disposable" "$runtime" deploy- || true
    fi
    if [[ $deploy_succeeded != true ]]; then
      write_status failed "$deploy_status_schema" || true
    fi
    exit "$exit_code"
  }
  trap record_deploy_failure EXIT
  active_state=$(systemctl is-active sanguinius.service 2>/dev/null || true)
  assert_process_state "$active_state"
  adopt_database_service_ownership "$database" sanguinius sanguinius
  verify_capacity
  [[ $(schema_version "$database") == "$expected_schema" ]] ||
    die "unexpected production schema"
  release_root=$(verify_archive)
  new_id=$(install_release "$release_root" true)
  new="$releases/$new_id"
  metadata_schema=$(sed -n 's/.*"schema_target":\([0-9]*\).*/\1/p' \
    "$new/RELEASE-METADATA.json")
  [[ $metadata_schema == "$target_schema" ]] || die "target schema mismatch"
  run_candidate_config_check "$new"
  if [[ -L $current ]]; then
    current_before_target=$(readlink -- "$current")
    [[ $current_before_target =~ ^releases/[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
       -d /opt/sanguinius/$current_before_target &&
       ! -L /opt/sanguinius/$current_before_target ]] ||
      die "current release pointer is unsafe"
    had_current_before=true
    old=$(readlink -f "$current")
  else
    [[ $expected_schema == 13 && -L $previous ]] ||
      die "current release state is inconsistent"
    old=$(readlink -f "$previous")
  fi
  [[ $old == "$releases/"* && -x $old/bin/sanguinius ]] || die "previous link is unsafe"
  old_schema=$(sed -n 's/.*"schema_target":\([0-9]*\).*/\1/p' \
    "$old/RELEASE-METADATA.json")
  [[ $old_schema == "$expected_schema" ]] || die "active release schema mismatch"
  old_unit=$(sed -n 's/.*"service_unit":"\([A-Za-z0-9.-]*\)".*/\1/p' \
    "$old/RELEASE-METADATA.json")
  [[ $old_unit == sanguinius.service || $old_unit == sanguinius-compat.service ]] ||
    die "active release service unit is invalid"
  cmp -s "$old/systemd/$old_unit" /etc/systemd/system/sanguinius.service ||
    die "installed service unit does not match the active release"
  if [[ -L $previous ]]; then
    previous_before_target=$(readlink -- "$previous")
    [[ $previous_before_target =~ ^releases/[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
       -d /opt/sanguinius/$previous_before_target &&
       ! -L /opt/sanguinius/$previous_before_target ]] ||
      die "previous release pointer is unsafe"
    had_previous_before=true
  elif [[ -e $previous ]]; then
    die "previous release pointer is unsafe"
  fi
  if [[ -L $operations ]]; then
    operations_before_target=$(readlink -- "$operations")
    [[ $operations_before_target =~ ^releases/[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ &&
       -d /opt/sanguinius/$operations_before_target &&
       ! -L /opt/sanguinius/$operations_before_target ]] ||
      die "operations release pointer is unsafe"
    had_operations_before=true
  elif [[ -e $operations ]]; then
    die "operations release pointer is unsafe"
  fi
  disposable=$(mktemp -d "$runtime/deploy-$new_id.XXXXXXXX")
  system_configuration_snapshot="$disposable/system-configuration-before"
  snapshot_system_configuration_files "$system_configuration_snapshot" ||
    die "system configuration could not be snapshotted"
  raw="$disposable/schema$expected_schema.sqlite3"
  verify_database_filesystem_entries "$database"
  SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db backup "$raw"
  SANGUINIUS_DATABASE_FILE="$raw" "$old/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$raw" "$old/bin/sanguinius" db relationships check
  SANGUINIUS_DATABASE_FILE="$raw" "$old/bin/sanguinius" db tarot check
  before_counts=$(safe_counts "$raw")
  old_revision=$(sed -n 's/.*"revision":"\([0-9a-f]*\)".*/\1/p' \
    "$old/RELEASE-METADATA.json")
  [[ $old_revision =~ ^[0-9a-f]{40}$ ]] || die "active release revision is invalid"
  rehearsal_archive="$disposable/schema$expected_schema-rehearsal.sqlite3.zst"
  zstd -q -19 -T1 -o "$rehearsal_archive" "$raw"
  zstd -q -t "$rehearsal_archive"
  rehearsal="$disposable/rehearsal.sqlite3"
  cp --reflink=auto "$raw" "$rehearsal"
  SANGUINIUS_DATABASE_FILE="$rehearsal" "$new/bin/sanguinius" db migrate
  SANGUINIUS_DATABASE_FILE="$rehearsal" "$new/bin/sanguinius" db check
  SANGUINIUS_DATABASE_FILE="$rehearsal" "$new/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$rehearsal" "$new/bin/sanguinius" db invariants check
  [[ $(safe_counts "$rehearsal") == "$before_counts" ]] ||
    die "migration rehearsal changed authoritative entity counts"
  rollback_migrated="$disposable/rollback-migrated.sqlite3"
  zstd -q -d -o "$rollback_migrated" "$rehearsal_archive"
  SANGUINIUS_DATABASE_FILE="$rollback_migrated" "$old/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$rollback_migrated" "$old/bin/sanguinius" db relationships check
  SANGUINIUS_DATABASE_FILE="$rollback_migrated" "$old/bin/sanguinius" db tarot check
  SANGUINIUS_DATABASE_FILE="$rollback_migrated" "$new/bin/sanguinius" db migrate
  SANGUINIUS_DATABASE_FILE="$rollback_migrated" "$new/bin/sanguinius" db check
  SANGUINIUS_DATABASE_FILE="$rollback_migrated" "$new/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$rollback_migrated" "$new/bin/sanguinius" db invariants check
  [[ $(safe_counts "$rollback_migrated") == "$before_counts" ]] ||
    die "rollback rehearsal changed authoritative entity counts"
  rollback_restored="$disposable/rollback-restored.sqlite3"
  zstd -q -d -o "$rollback_restored" "$rehearsal_archive"
  SANGUINIUS_DATABASE_FILE="$rollback_restored" "$old/bin/sanguinius" db migrate
  SANGUINIUS_DATABASE_FILE="$rollback_restored" "$old/bin/sanguinius" db check
  SANGUINIUS_DATABASE_FILE="$rollback_restored" "$old/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$rollback_restored" "$old/bin/sanguinius" db relationships check
  SANGUINIUS_DATABASE_FILE="$rollback_restored" "$old/bin/sanguinius" db tarot check
  [[ $(safe_counts "$rollback_restored") == "$before_counts" ]] ||
    die "restored schema-13 backup changed authoritative entity counts"
  if [[ $active_state == active ]]; then
    systemctl stop sanguinius.service
    service_stopped_for_deploy=true
  fi
  assert_process_state inactive
  assert_database_exclusive
  database_exclusive_for_deploy=true
  verify_database_filesystem_entries "$database"
  authoritative_raw="$disposable/pre-migration-authoritative.sqlite3"
  SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db backup \
    "$authoritative_raw"
  SANGUINIUS_DATABASE_FILE="$authoritative_raw" \
    "$old/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$authoritative_raw" \
    "$old/bin/sanguinius" db relationships check
  SANGUINIUS_DATABASE_FILE="$authoritative_raw" \
    "$old/bin/sanguinius" db tarot check
  authoritative_counts=$(safe_counts "$authoritative_raw")
  final_migration_rehearsal="$disposable/final-migration-rehearsal.sqlite3"
  cp --reflink=auto "$authoritative_raw" "$final_migration_rehearsal"
  SANGUINIUS_DATABASE_FILE="$final_migration_rehearsal" \
    "$new/bin/sanguinius" db migrate
  SANGUINIUS_DATABASE_FILE="$final_migration_rehearsal" \
    "$new/bin/sanguinius" db check
  SANGUINIUS_DATABASE_FILE="$final_migration_rehearsal" \
    "$new/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$final_migration_rehearsal" \
    "$new/bin/sanguinius" db invariants check
  [[ $(safe_counts "$final_migration_rehearsal") == "$authoritative_counts" ]] ||
    die "final migration rehearsal changed authoritative entity counts"
  pre="$backups/$(date -u +%Y%m%dT%H%M%SZ)-schema$expected_schema-${old_revision:0:12}-pre-migration.sqlite3.zst"
  [[ ! -e $pre && ! -L $pre ]] || die "pre-migration backup exists"
  zstd -q -19 -T1 -o "$pre" "$authoritative_raw"
  zstd -q -t "$pre"
  final_rollback_restore="$disposable/final-rollback-restore.sqlite3"
  zstd -q -d -o "$final_rollback_restore" "$pre"
  SANGUINIUS_DATABASE_FILE="$final_rollback_restore" \
    "$old/bin/sanguinius" db migrate
  SANGUINIUS_DATABASE_FILE="$final_rollback_restore" \
    "$old/bin/sanguinius" db check
  SANGUINIUS_DATABASE_FILE="$final_rollback_restore" \
    "$old/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$final_rollback_restore" \
    "$old/bin/sanguinius" db relationships check
  SANGUINIUS_DATABASE_FILE="$final_rollback_restore" \
    "$old/bin/sanguinius" db tarot check
  [[ $(safe_counts "$final_rollback_restore") == "$authoritative_counts" ]] ||
    die "authoritative rollback backup changed entity counts"
  old_release_id=$(sed -n \
    's/.*"release_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' \
    "$old/RELEASE-METADATA.json")
  publish_backup_sidecars "$pre" "$authoritative_raw" "$expected_schema" \
    "$old_revision" "$old_release_id" "$(basename "$old")" pre-migration
  SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db migrate || {
    [[ $(schema_version "$database") == "$expected_schema" ]] || die "migration transaction did not roll back"
    SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db integrity
    SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db relationships check
    SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db tarot check
    select_release_for_schema "$old" "$expected_schema"
    restart_if_previously_active "$active_state"
    die "production migration failed"
  }
  production_migration_completed=true
  deploy_status_schema=$target_schema
  if ! SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db check ||
     ! SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db integrity ||
     ! SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db invariants check; then
    failed="$runtime/failed-$new_id.sqlite3"
    SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db backup "$failed" || true
    recovery_failed=false
    recover_predeployment_durable_state true || recovery_failed=true
    candidate_failure_handled=true
    if [[ $recovery_failed == true ]]; then
      die "post-migration verification failed; recovery was incomplete and the service was fenced"
    fi
    die "post-migration verification failed; schema-13 backup restored"
  fi
  candidate_durable_setup_started=true
  atomic_link "releases/$new_id" "$current"
  inject_deploy_setup_failure current-link
  install_release_units "$new"
  inject_deploy_setup_failure release-units
  install_system_configuration_files "$new"
  inject_deploy_setup_failure system-configuration
  systemctl daemon-reload
  inject_deploy_setup_failure daemon-reload
  [[ ! -e $operations || -L $operations ]] ||
    die "operations release pointer is unsafe"
  atomic_link "releases/$new_id" "$operations"
  inject_deploy_setup_failure operations-link
  candidate_restart_baseline=$(systemctl show -p NRestarts --value \
    sanguinius.service)
  [[ $candidate_restart_baseline =~ ^[0-9]+$ ]] ||
    die "candidate restart baseline is unavailable"
  systemctl start sanguinius.service || {
    main_timestamp=$(systemctl show -p ExecMainStartTimestampMonotonic --value \
      sanguinius.service)
    [[ $main_timestamp =~ ^[1-9][0-9]*$ ]] && candidate_main_started=true
    action=$(failure_action "$expected_schema" "$target_schema" \
      "$candidate_main_started")
    status_schema=$target_schema
    failure_message="candidate reached its main process; schema and diagnostics are preserved"
    recovery_failed=false
    if [[ $action == rollback-release ]]; then
      recover_predeployment_durable_state false || recovery_failed=true
      failure_message="same-schema candidate failed; pre-deployment state was restored"
    elif [[ $action == restore-database ]]; then
      failed="$runtime/failed-$new_id.sqlite3"
      SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db backup "$failed" || true
      if recover_predeployment_durable_state true; then
        status_schema=$expected_schema
      else
        recovery_failed=true
      fi
      failure_message="candidate failed before its main process; pre-deployment state was restored"
    else
      if ! fence_schema_changing_candidate; then
        retain_deploy_diagnostics=true
        recovery_failed=true
      fi
    fi
    candidate_failure_handled=true
    if [[ $recovery_failed == true ]]; then
      failure_message="candidate failure recovery was incomplete; the service was fenced"
    fi
    write_status failed "$status_schema"
    die "$failure_message"
  }
  candidate_main_started=true
  if ! candidate_service_health_verified "$candidate_restart_baseline"; then
    if [[ $expected_schema == "$target_schema" ]]; then
      recovery_failed=false
      recover_predeployment_durable_state false || recovery_failed=true
      candidate_failure_handled=true
      if [[ $recovery_failed == true ]]; then
        die "same-schema candidate failed readiness; recovery was incomplete and the service was fenced"
      fi
      die "same-schema candidate failed readiness; pre-deployment state was restored"
    fi
    recovery_failed=false
    if ! fence_schema_changing_candidate; then
      retain_deploy_diagnostics=true
      recovery_failed=true
    fi
    candidate_failure_handled=true
    if [[ $recovery_failed == true ]]; then
      die "schema-changing candidate fencing could not be verified; manual intervention is required"
    fi
    die "schema-changing candidate failed readiness; schema and diagnostics are preserved"
  fi
  candidate_gateway_ready=true
  SANGUINIUS_LOCK_HELD=true \
    "$operations/libexec/sanguinius-backup.bash" manual --lock-held
  atomic_link "releases/$(basename "$old")" "$previous"
  systemctl enable sanguinius.service
  systemctl enable --now sanguinius-backup.timer
  write_state_version "$state" "$runtime" "$target_schema"
  current_id=$(basename "$(readlink -f "$current")")
  previous_id=$(basename "$(readlink -f "$previous")")
  protected_releases=()
  [[ $had_previous_before != true ]] ||
    protected_releases+=("${previous_before_target#releases/}")
  [[ $had_current_before != true ]] ||
    protected_releases+=("${current_before_target#releases/}")
  [[ $had_operations_before != true ]] ||
    protected_releases+=("${operations_before_target#releases/}")
  prune_recognized_releases "$releases" "$current_id" "$previous_id" \
    "${protected_releases[@]}" || die "pre-commit release retention failed"
  candidate_service_health_verified "$candidate_restart_baseline" ||
    die "candidate lost readiness or restarted during deployment finalization"
  remove_managed_tree "$disposable" "$runtime" deploy-
  disposable=
  write_status succeeded "$target_schema"
  deploy_succeeded=true
  if ! prune_recognized_releases "$releases" "$current_id" "$previous_id"; then
    echo "WARNING: Deployment succeeded, but final release retention was incomplete." >&2
  fi
  echo "deploy=complete"
  echo "release=$new_id"
  ;;
rollback-same-schema)
  managed_release_name "$release_id" || die "release ID invalid"
  [[ $(stat -c '%U:%G:%a' "$runtime") == sanguinius:sanguinius:700 ||
     $(stat -c '%U:%G:%a' "$runtime") == root:sanguinius:750 ]] ||
    die "operations directory cannot be safely upgraded"
  install -d -m 0750 -o root -g sanguinius "$runtime"
  open_operations_lock "$runtime" "$lock_file"
  verify_database_filesystem_entries "$database"
  target="$releases/$release_id"
  [[ -d $target && ! -L $target && -x $target/bin/sanguinius ]] || die "release unavailable"
  verify_payload_tree "$target"
  verify_release_binary_identity "$target"
  [[ $(metadata_deployment_id "$target/RELEASE-METADATA.json") == "$release_id" ]] ||
    die "rollback deployment ID conflicts"
  target_schema=$(sed -n 's/.*"schema_target":\([0-9]*\).*/\1/p' "$target/RELEASE-METADATA.json")
  [[ $(schema_version "$database") == "$target_schema" ]] || die "schema mismatch"
  [[ -L $current ]] || die "current release pointer is unavailable"
  old=$(readlink -f -- "$current")
  [[ $old == "$releases/"* && $(dirname "$old") == "$releases" &&
     -d $old && ! -L $old && -x $old/bin/sanguinius ]] ||
    die "current release pointer is unsafe"
  old_id=$(basename "$old")
  managed_release_directory "$old" "$old_id" ||
    die "current release identity is unsafe"
  require_distinct_rollback_target "$release_id" "$old_id"
  systemctl stop sanguinius.service
  assert_process_state inactive
  assert_database_exclusive
  atomic_link "releases/$release_id" "$current"
  unit=$(sed -n 's/.*"service_unit":"\([A-Za-z0-9.-]*\)".*/\1/p' "$target/RELEASE-METADATA.json")
  install -m 0644 "$target/systemd/$unit" /etc/systemd/system/sanguinius.service
  systemctl daemon-reload
  if ! start_sanguinius_service_verified; then
    atomic_link "releases/$old_id" "$current"
    install_service_unit "$releases/$old_id"
    systemctl daemon-reload
    start_sanguinius_service_verified ||
      die "same-schema rollback target and prior release both failed to start"
    die "same-schema rollback target failed"
  fi
  atomic_link "releases/$old_id" "$previous"
  echo "rollback=complete"
  echo "release=$release_id"
  ;;
*) die "unknown operation" ;;
esac
