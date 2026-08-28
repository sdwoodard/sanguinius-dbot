#!/usr/bin/env bash
set -euo pipefail
umask 0077

repository=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
host=nuln
helper="$repository/scripts/lib/remote_deploy.bash"

usage() {
  echo "Usage: $0 inspect" >&2
  echo "       $0 bootstrap --rollback-archive <archive> --environment <remote-path> --token <remote-path> --openai-key <remote-path> --message-log <remote-path>" >&2
  echo "       $0 deploy --archive <archive> --expected-schema <n> --target-schema <n>" >&2
  echo "       $0 rollback-same-schema --release-id <id>" >&2
  exit 2
}

[[ -x $helper || -f $helper ]] || {
  echo "Remote deployment helper is missing." >&2
  exit 1
}
command=${1:-}
shift || true

if [[ $command == inspect ]]; then
  (( $# == 0 )) || usage
  ssh "$host" 'set -eu; test "$(hostname -s)" = nuln; uname -m; systemctl is-active sanguinius.service 2>/dev/null || true; pgrep -a -x sanguinius 2>/dev/null || true; df -Pk /var/lib/sanguinius /var/backups/sanguinius 2>/dev/null || true'
  exit 0
fi

archive=
expected_schema=
target_schema=
release_id=
environment=
token=
openai_key=
message_log=
while (( $# )); do
  case "$1" in
    --archive|--rollback-archive) (( $# >= 2 )) || usage; archive=$2; shift 2 ;;
    --expected-schema) (( $# >= 2 )) || usage; expected_schema=$2; shift 2 ;;
    --target-schema) (( $# >= 2 )) || usage; target_schema=$2; shift 2 ;;
    --release-id) (( $# >= 2 )) || usage; release_id=$2; shift 2 ;;
    --environment) (( $# >= 2 )) || usage; environment=$2; shift 2 ;;
    --token) (( $# >= 2 )) || usage; token=$2; shift 2 ;;
    --openai-key) (( $# >= 2 )) || usage; openai_key=$2; shift 2 ;;
    --message-log) (( $# >= 2 )) || usage; message_log=$2; shift 2 ;;
    *) usage ;;
  esac
done

if [[ $command == rollback-same-schema ]]; then
  [[ $release_id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ ]] || usage
  rollback_directory=$(ssh "$host" \
    'umask 077; mktemp -d /tmp/sanguinius-upload.XXXXXXXX')
  [[ $rollback_directory =~ ^/tmp/sanguinius-upload\.[A-Za-z0-9]{8}$ ]] || exit 1
  # shellcheck disable=SC2329
  cleanup_rollback() {
    # rollback_directory is a validated fixed-format remote path.
    # shellcheck disable=SC2029
    ssh "$host" "test -d '$rollback_directory' && find '$rollback_directory' -maxdepth 1 -type f -delete; rmdir '$rollback_directory' 2>/dev/null || true" \
      >/dev/null 2>&1 || true
  }
  trap cleanup_rollback EXIT
  scp -q "$helper" "$host:$rollback_directory/remote_deploy.bash"
  helper_sha=$(sha256sum "$helper" | awk '{print $1}')
  ssh -t "$host" sudo /bin/bash "$rollback_directory/remote_deploy.bash" \
    rollback-same-schema --expected-helper-sha "$helper_sha" \
    --release-id "$release_id"
  exit 0
fi

[[ $command == bootstrap || $command == deploy ]] || usage
[[ $archive == /* && -f $archive && ! -L $archive ]] || usage
"$repository/scripts/release.bash" verify --archive "$archive"
[[ -z $(git -C "$repository" status --porcelain=v1 --untracked-files=all) ]] || {
  echo "Deployment requires a clean local tree." >&2
  exit 1
}
archive_root=$(tar --zstd -tf "$archive" | sed -n '1s,/.*,,p')
metadata=$(tar --zstd -xOf "$archive" \
  "$archive_root/RELEASE-METADATA.json")
metadata_revision=$(sed -n 's/.*"revision":"\([0-9a-f]\{40\}\)".*/\1/p' \
  <<<"$metadata")
metadata_release=$(sed -n 's/.*"release_id":"\([A-Za-z0-9._+-]*\)".*/\1/p' \
  <<<"$metadata")
evidence="${archive%.tar.zst}.test-evidence.json"
[[ -f $evidence && ! -L $evidence ]] || {
  echo "Matching container test evidence is missing." >&2
  exit 1
}
archive_sha=$(sha256sum "$archive" | awk '{print $1}')
if ! grep -Fq '"revision":"'"$metadata_revision"'"' "$evidence" ||
   ! grep -Fq '"release_id":"'"$metadata_release"'"' "$evidence" ||
   ! grep -Fq '"archive_sha256":"'"$archive_sha"'"' "$evidence" ||
   ! grep -Fq '"container_release_build":"passed"' "$evidence" ||
   ! grep -Fq '"container_ctest":"passed"' "$evidence"; then
    echo "Container test evidence does not match the archive." >&2
    exit 1
fi

remote_directory=$(ssh "$host" 'umask 077; mktemp -d /tmp/sanguinius-upload.XXXXXXXX')
[[ $remote_directory =~ ^/tmp/sanguinius-upload\.[A-Za-z0-9]{8}$ ]] || exit 1
# shellcheck disable=SC2329
cleanup() {
  # remote_directory is a validated fixed-format remote path.
  # shellcheck disable=SC2029
  ssh "$host" "test -d '$remote_directory' && find '$remote_directory' -maxdepth 1 -type f -delete; rmdir '$remote_directory' 2>/dev/null || true" >/dev/null 2>&1 || true
}
trap cleanup EXIT
scp -q "$archive" "$archive.sha256" "$helper" "$host:$remote_directory/"
archive_remote="$remote_directory/$(basename "$archive")"
helper_remote="$remote_directory/remote_deploy.bash"
helper_sha=$(sha256sum "$helper" | awk '{print $1}')

if [[ $command == bootstrap ]]; then
  [[ -n $environment && -n $token && -n $openai_key && -n $message_log ]] || usage
  ssh -t "$host" sudo /bin/bash "$helper_remote" bootstrap \
    --expected-helper-sha "$helper_sha" --archive "$archive_remote" \
    --expected-archive-sha "$archive_sha" --environment "$environment" \
    --token "$token" --openai-key "$openai_key" --message-log "$message_log"
else
  [[ $expected_schema =~ ^[0-9]+$ && $target_schema =~ ^[0-9]+$ ]] || usage
  [[ $metadata_revision == "$(git -C "$repository" rev-parse HEAD)" ]] || {
    echo "Deployment archive does not match local HEAD." >&2
    exit 1
  }
  ssh -t "$host" sudo /bin/bash "$helper_remote" deploy \
    --expected-helper-sha "$helper_sha" --archive "$archive_remote" \
    --expected-archive-sha "$archive_sha" \
    --expected-schema "$expected_schema" --target-schema "$target_schema"
fi
