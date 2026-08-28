#!/usr/bin/env bash
set -euo pipefail
umask 0077

die() { echo "Deployment refused: $*" >&2; exit 1; }

atomic_link() {
  local target=$1 link=$2 temporary
  temporary="$link.new.$$"
  ln -s "$target" "$temporary"
  mv -Tf "$temporary" "$link"
}

managed_release_name() {
  [[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ ]]
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
    policy-failure)
      [[ $# -eq 3 ]] || exit 2
      failure_action "$@"
      exit ;;
    policy-atomic-link)
      [[ $# -eq 2 && -n ${SANGUINIUS_TEST_ROOT:-} &&
         $2 == "$SANGUINIUS_TEST_ROOT"/* ]] || exit 2
      atomic_link "$1" "$2"
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

assert_database_exclusive() {
  local fd target
  for fd in /proc/[0-9]*/fd/[0-9]*; do
    [[ -e $fd ]] || continue
    target=$(readlink -f "$fd" 2>/dev/null || true)
    case "$target" in
      "$database"|"$database-wal"|"$database-shm"|"$database-journal")
        die "database is still open by another process" ;;
    esac
  done
}

safe_counts() {
  sqlite3 -readonly "$1" \
    'SELECT (SELECT count(*) FROM discord_user)||":"||(SELECT count(*) FROM chronicle_entry)||":"||(SELECT count(*) FROM relationship_event)||":"||(SELECT count(*) FROM tarot_transaction)||":"||(SELECT count(*) FROM tarot_posting)||":"||(SELECT count(*) FROM tarot_wager)||":"||(SELECT count(*) FROM voice_session)||":"||(SELECT count(*) FROM speech_item);'
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

install_release() {
  local root=$1
  local id=${root#sanguinius-}
  local incoming="$releases/.incoming-$id"
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
  [[ ! -e $releases/$id && ! -e $incoming ]] || die "release ID already exists"
  mkdir -m 0700 "$incoming"
  tar --zstd -xf "$archive" -C "$incoming" --strip-components=1 \
    --no-same-owner --no-same-permissions
  [[ -f $incoming/RELEASE-METADATA.json &&
     -f $incoming/SHARE-MANIFEST.sha256 &&
     -x $incoming/bin/sanguinius &&
     -f $incoming/lib/libdpp.so.10.1.7 ]] || die "release payload incomplete"
  verify_payload_tree "$incoming"
  local embedded_id
  embedded_id=$(sed -n 's/.*"release_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' \
    "$incoming/RELEASE-METADATA.json")
  [[ $embedded_id == "$id" ]] || die "conflicting release ID"
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
  local maximum_glibc maximum_glibcxx compatibility version_json
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
  compatibility=$(sed -n 's/.*"compatibility_release":\(true\|false\).*/\1/p' \
    "$incoming/RELEASE-METADATA.json")
  if [[ $compatibility == false ]]; then
    version_json=$("$incoming/bin/sanguinius" --version --json) ||
      die "release self-identification failed"
    [[ $version_json == *'"release_id":"'"$id"'"'* ]] ||
      die "binary and release metadata disagree"
  else
    [[ $compatibility == true ]] || die "compatibility metadata is invalid"
  fi
  find "$incoming" -type d -exec chmod 0755 '{}' +
  find "$incoming" -type f -exec chmod 0444 '{}' +
  chmod 0555 "$incoming/bin/sanguinius" "$incoming/lib/libdpp.so.10.1.7"
  [[ ! -d $incoming/libexec ]] || chmod 0555 "$incoming/libexec"/*
  chown -R root:root "$incoming"
  mv -T "$incoming" "$releases/$id"
  incoming_committed=true
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

install_service_unit() {
  local release=$1 unit
  unit=$(sed -n 's/.*"service_unit":"\([A-Za-z0-9.-]*\)".*/\1/p' \
    "$release/RELEASE-METADATA.json")
  [[ $unit == sanguinius.service || $unit == sanguinius-compat.service ]] ||
    die "release service unit is invalid"
  [[ -f $release/systemd/$unit && ! -L $release/systemd/$unit ]] ||
    die "release service unit is unavailable"
  install -m 0644 "$release/systemd/$unit" \
    /etc/systemd/system/sanguinius.service
}

case "$operation" in
bootstrap)
  [[ -n $archive && -n $environment && -n $token && -n $openai_key &&
     -n $message_log ]] || die "bootstrap arguments are incomplete"
  active_state=$(systemctl is-active sanguinius.service 2>/dev/null || true)
  assert_process_state "$active_state"
  [[ $active_state != active ]] ||
    die "service must be inactive"
  [[ -f $database && ! -L $database && $(schema_version "$database") == 13 ]] ||
    die "expected schema-13 database is unavailable"
  verify_secret_source "$environment"
  verify_secret_source "$token"
  verify_secret_source "$openai_key"
  [[ $message_log == /* && -f $message_log && ! -L $message_log ]] ||
    die "message log source is unsafe"
  ! grep -Eq '^(SANGUINIUS_TOKEN|OPENAI_API_KEY)=' "$environment" ||
    die "environment file contains a credential value"
  grep -qx 'SANGUINIUS_ADMIN_COMMANDS_ENABLED=false' "$environment" ||
    die "admin commands must be disabled at bootstrap"
  grep -qx 'SANGUINIUS_TEST_MODE=false' "$environment" ||
    die "test mode must be disabled at bootstrap"
  grep -qx 'SANGUINIUS_VOICE_INPUT_ENABLED=false' "$environment" ||
    die "voice input must be disabled at bootstrap"
  grep -qx 'SANGUINIUS_VOICE_INPUT_GUILD_CONSENT_ATTESTED=false' "$environment" ||
    die "voice input consent must be disabled at bootstrap"
  grep -qx 'SANGUINIUS_TRANSCRIPTION_PROVIDER=disabled' "$environment" ||
    die "transcription must be disabled at bootstrap"
  grep -qx 'SANGUINIUS_CHRONICLE_ENABLED=true' "$environment" ||
    die "Chronicle must be enabled at bootstrap"
  grep -qx 'SANGUINIUS_TAROT_ENABLED=true' "$environment" ||
    die "Tarot must be enabled at bootstrap"
  grep -qx 'SANGUINIUS_VOX_ENABLED=true' "$environment" ||
    die "Vox output must be enabled at bootstrap"
  grep -qx 'SANGUINIUS_VOX_NARRATION_ENABLED=true' "$environment" ||
    die "Vox narration must be enabled at bootstrap"
  grep -qx 'SANGUINIUS_TTS_PROVIDER=openai' "$environment" ||
    die "TTS must be enabled at bootstrap"
  grep -qx 'SANGUINIUS_APPEARANCES_MODE=dry_run' "$environment" ||
    die "appearances must begin in dry-run mode"
  release_root=$(verify_archive)
  if ! getent passwd sanguinius >/dev/null; then
    systemd-sysusers --inline \
      'u sanguinius - "Sanguinius Discord bot" /var/lib/sanguinius /usr/bin/nologin'
  fi
  install -d -m 0700 -o root -g root /etc/sanguinius "$backups"
  install -d -m 0755 -o root -g root /opt/sanguinius "$releases"
  install -d -m 0750 -o sanguinius -g sanguinius "$state"
  install -d -m 0700 -o sanguinius -g sanguinius "$runtime" \
    /var/cache/sanguinius /var/cache/sanguinius/tts /var/log/sanguinius
  verify_capacity
  exec 9>"$lock_file"
  flock -n 9 || die "operations lock is held"
  rollback_id=$(install_release "$release_root")
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
  compressed_sha=$(sha256sum "$compressed" | awk '{print $1}')
  printf '%s  %s\n' "$compressed_sha" "$(basename "$compressed")" \
    >"$compressed.sha256"
  printf '{"format_version":1,"class":"pre-migration","schema":13,"revision":"%s","compressed_sha256":"%s","integrity":"ok","relationships":"ok","tarot":"ok","restore_copy":"ok"}\n' \
    "$rollback_revision" "$compressed_sha" >"${compressed%.zst}.json"
  rm -f -- "$raw" "$restored"
  install -m 0600 -o root -g root "$environment" /etc/sanguinius/sanguinius.env
  install -m 0600 -o root -g root "$token" /etc/sanguinius/bot.token
  install -m 0600 -o root -g root "$openai_key" /etc/sanguinius/openai.key
  install -m 0600 -o sanguinius -g sanguinius "$message_log" \
    /var/log/sanguinius/messages.log
  chown sanguinius:sanguinius "$database" "$database-wal" "$database-shm" 2>/dev/null || true
  chmod 0600 "$database" "$database-wal" "$database-shm" 2>/dev/null || true
  printf '13\n' >/var/lib/sanguinius/state-version
  chown sanguinius:sanguinius /var/lib/sanguinius/state-version
  atomic_link "releases/$rollback_id" "$previous"
  install_service_unit "$rollback"
  install -m 0644 "$rollback/sysusers.d/sanguinius.conf" \
    /etc/sysusers.d/sanguinius.conf
  install -m 0644 "$rollback/tmpfiles.d/sanguinius.conf" \
    /etc/tmpfiles.d/sanguinius.conf
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
  exec 9>"$lock_file"
  flock -n 9 || { echo "Another deployment or backup is active." >&2; exit 75; }
  write_status running "$expected_schema"
  deploy_succeeded=false
  deploy_status_schema=$expected_schema
  # Invoked indirectly by the EXIT trap.
  # shellcheck disable=SC2329
  record_deploy_failure() {
    local exit_code=$?
    if [[ $deploy_succeeded != true ]]; then
      write_status failed "$deploy_status_schema" || true
    fi
    exit "$exit_code"
  }
  trap record_deploy_failure EXIT
  active_state=$(systemctl is-active sanguinius.service 2>/dev/null || true)
  assert_process_state "$active_state"
  verify_capacity
  [[ $(schema_version "$database") == "$expected_schema" ]] ||
    die "unexpected production schema"
  release_root=$(verify_archive)
  new_id=$(install_release "$release_root")
  new="$releases/$new_id"
  metadata_schema=$(sed -n 's/.*"schema_target":\([0-9]*\).*/\1/p' \
    "$new/RELEASE-METADATA.json")
  [[ $metadata_schema == "$target_schema" ]] || die "target schema mismatch"
  if [[ -L $current ]]; then
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
  disposable=$(mktemp -d "$runtime/deploy-$new_id.XXXXXXXX")
  raw="$disposable/schema$expected_schema.sqlite3"
  SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db backup "$raw"
  SANGUINIUS_DATABASE_FILE="$raw" "$old/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$raw" "$old/bin/sanguinius" db relationships check
  SANGUINIUS_DATABASE_FILE="$raw" "$old/bin/sanguinius" db tarot check
  before_counts=$(safe_counts "$raw")
  old_revision=$(sed -n 's/.*"revision":"\([0-9a-f]*\)".*/\1/p' \
    "$old/RELEASE-METADATA.json")
  [[ $old_revision =~ ^[0-9a-f]{40}$ ]] || die "active release revision is invalid"
  pre="$backups/$(date -u +%Y%m%dT%H%M%SZ)-schema$expected_schema-${old_revision:0:12}-pre-migration.sqlite3.zst"
  [[ ! -e $pre ]] || die "pre-migration backup exists"
  zstd -q -19 -T1 -o "$pre" "$raw"
  zstd -q -t "$pre"
  pre_sha=$(sha256sum "$pre" | awk '{print $1}')
  printf '%s  %s\n' "$pre_sha" "$(basename "$pre")" >"$pre.sha256"
  printf '{"format_version":1,"class":"pre-migration","schema":%s,"revision":"%s","compressed_sha256":"%s","integrity":"ok","domain_invariants":"ok","restore_copy":"ok"}\n' \
    "$expected_schema" "$old_revision" "$pre_sha" >"${pre%.zst}.json"
  rehearsal="$disposable/rehearsal.sqlite3"
  cp --reflink=auto "$raw" "$rehearsal"
  SANGUINIUS_DATABASE_FILE="$rehearsal" "$new/bin/sanguinius" db migrate
  SANGUINIUS_DATABASE_FILE="$rehearsal" "$new/bin/sanguinius" db check
  SANGUINIUS_DATABASE_FILE="$rehearsal" "$new/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$rehearsal" "$new/bin/sanguinius" db invariants check
  [[ $(safe_counts "$rehearsal") == "$before_counts" ]] ||
    die "migration rehearsal changed authoritative entity counts"
  rollback_migrated="$disposable/rollback-migrated.sqlite3"
  zstd -q -d -o "$rollback_migrated" "$pre"
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
  zstd -q -d -o "$rollback_restored" "$pre"
  SANGUINIUS_DATABASE_FILE="$rollback_restored" "$old/bin/sanguinius" db migrate
  SANGUINIUS_DATABASE_FILE="$rollback_restored" "$old/bin/sanguinius" db check
  SANGUINIUS_DATABASE_FILE="$rollback_restored" "$old/bin/sanguinius" db integrity
  SANGUINIUS_DATABASE_FILE="$rollback_restored" "$old/bin/sanguinius" db relationships check
  SANGUINIUS_DATABASE_FILE="$rollback_restored" "$old/bin/sanguinius" db tarot check
  [[ $(safe_counts "$rollback_restored") == "$before_counts" ]] ||
    die "restored schema-13 backup changed authoritative entity counts"
  if [[ $active_state == active ]]; then
    systemctl stop sanguinius.service
  fi
  assert_process_state inactive
  assert_database_exclusive
  SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db migrate || {
    [[ $(schema_version "$database") == "$expected_schema" ]] || die "migration transaction did not roll back"
    SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db integrity
    SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db relationships check
    SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db tarot check
    [[ $active_state != active ]] || systemctl start sanguinius.service
    die "production migration failed"
  }
  deploy_status_schema=$target_schema
  if ! SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db check ||
     ! SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db integrity ||
     ! SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db invariants check; then
    failed="$runtime/failed-$new_id.sqlite3"
    SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db backup "$failed" || true
    systemctl stop sanguinius.service 2>/dev/null || true
    for suffix in '' -wal -shm -journal; do rm -f -- "$database$suffix"; done
    zstd -q -d -o "$database" "$pre"
    chown sanguinius:sanguinius "$database"
    SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db migrate
    SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db integrity
    SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db relationships check
    SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db tarot check
    [[ $active_state != active ]] || systemctl start sanguinius.service
    deploy_status_schema=$expected_schema
    die "post-migration verification failed; schema-13 backup restored"
  fi
  atomic_link "releases/$new_id" "$current"
  install_service_unit "$new"
  install -m 0644 "$new/systemd/sanguinius-backup.service" \
    /etc/systemd/system/sanguinius-backup.service
  install -m 0644 "$new/systemd/sanguinius-backup.timer" \
    /etc/systemd/system/sanguinius-backup.timer
  install -m 0644 "$new/sysusers.d/sanguinius.conf" \
    /etc/sysusers.d/sanguinius.conf
  install -m 0644 "$new/tmpfiles.d/sanguinius.conf" \
    /etc/tmpfiles.d/sanguinius.conf
  systemctl daemon-reload
  systemctl start sanguinius.service || {
    systemctl stop sanguinius.service 2>/dev/null || true
    main_started=false
    main_timestamp=$(systemctl show -p ExecMainStartTimestampMonotonic --value \
      sanguinius.service)
    [[ $main_timestamp =~ ^[1-9][0-9]*$ ]] && main_started=true
    action=$(failure_action "$expected_schema" "$target_schema" "$main_started")
    status_schema=$target_schema
    failure_message="candidate reached its main process; schema and diagnostics are preserved"
    if [[ $action == rollback-release ]]; then
      atomic_link "releases/$(basename "$old")" "$current"
      install_service_unit "$old"
      systemctl daemon-reload
      systemctl start sanguinius.service || true
      failure_message="same-schema candidate failed; previous release was restored"
    elif [[ $action == restore-database ]]; then
      failed="$runtime/failed-$new_id.sqlite3"
      SANGUINIUS_DATABASE_FILE="$database" "$new/bin/sanguinius" db backup "$failed" || true
      for suffix in '' -wal -shm -journal; do rm -f -- "$database$suffix"; done
      zstd -q -d -o "$database" "$pre"
      chown sanguinius:sanguinius "$database"
      atomic_link "releases/$(basename "$old")" "$current"
      install_service_unit "$old"
      systemctl daemon-reload
      SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db integrity
      SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db relationships check
      SANGUINIUS_DATABASE_FILE="$database" "$old/bin/sanguinius" db tarot check
      status_schema=$expected_schema
      deploy_status_schema=$expected_schema
      failure_message="candidate failed before its main process; verified backup was restored"
    fi
    write_status failed "$status_schema"
    die "$failure_message"
  }
  if [[ $(systemctl show -p ActiveState --value sanguinius.service) != active ||
        $(systemctl show -p StatusText --value sanguinius.service) != \
          'Ready; Discord connected and commands synchronized' ]]; then
    systemctl stop sanguinius.service 2>/dev/null || true
    if [[ $expected_schema == "$target_schema" ]]; then
      atomic_link "releases/$(basename "$old")" "$current"
      install_service_unit "$old"
      systemctl daemon-reload
      systemctl start sanguinius.service || true
      deploy_status_schema=$expected_schema
      die "same-schema candidate failed readiness; previous release was restored"
    fi
    die "schema-changing candidate failed readiness; schema and diagnostics are preserved"
  fi
  atomic_link "releases/$(basename "$old")" "$previous"
  systemctl enable --now sanguinius-backup.timer
  printf '%s\n' "$target_schema" >$state/state-version
  chown sanguinius:sanguinius $state/state-version
  mapfile -t inactive < <(
    find "$releases" -mindepth 1 -maxdepth 1 -type d \
      ! -name '.incoming-*' -printf '%f\n' | LC_ALL=C sort -r
  )
  current_id=$(basename "$(readlink -f "$current")")
  previous_id=$(basename "$(readlink -f "$previous")")
  mapfile -t expired < <(
    retention_deletions "$current_id" "$previous_id" "${inactive[@]}"
  )
  for candidate in "${expired[@]}"; do
      candidate_path="$releases/$candidate"
      [[ -d $candidate_path && ! -L $candidate_path ]] || continue
      [[ -f $candidate_path/RELEASE-METADATA.json ]] || continue
      candidate_metadata_id=$(sed -n 's/.*"release_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' \
        "$candidate_path/RELEASE-METADATA.json")
      [[ $candidate_metadata_id == "$candidate" ]] || continue
      find "$candidate_path" -depth -type f -delete
      find "$candidate_path" -depth -type d -empty -delete
  done
  find "$disposable" -maxdepth 1 -type f -delete
  rmdir "$disposable"
  write_status succeeded "$target_schema" backup-succeeded
  deploy_succeeded=true
  echo "deploy=complete"
  echo "release=$new_id"
  ;;
rollback-same-schema)
  managed_release_name "$release_id" || die "release ID invalid"
  exec 9>"$lock_file"; flock -n 9 || exit 75
  target="$releases/$release_id"
  [[ -d $target && ! -L $target && -x $target/bin/sanguinius ]] || die "release unavailable"
  target_schema=$(sed -n 's/.*"schema_target":\([0-9]*\).*/\1/p' "$target/RELEASE-METADATA.json")
  [[ $(schema_version "$database") == "$target_schema" ]] || die "schema mismatch"
  systemctl stop sanguinius.service
  old_id=$(basename "$(readlink -f "$current")")
  atomic_link "releases/$release_id" "$current"
  unit=$(sed -n 's/.*"service_unit":"\([A-Za-z0-9.-]*\)".*/\1/p' "$target/RELEASE-METADATA.json")
  install -m 0644 "$target/systemd/$unit" /etc/systemd/system/sanguinius.service
  systemctl daemon-reload
  if ! systemctl start sanguinius.service; then
    atomic_link "releases/$old_id" "$current"
    install_service_unit "$releases/$old_id"
    systemctl daemon-reload
    systemctl start sanguinius.service || true
    die "same-schema rollback target failed"
  fi
  atomic_link "releases/$old_id" "$previous"
  echo "rollback=complete"
  echo "release=$release_id"
  ;;
*) die "unknown operation" ;;
esac
