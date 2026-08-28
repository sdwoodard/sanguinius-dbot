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
  'SANGUINIUS_VOX_ENABLED=true' \
  'SANGUINIUS_VOX_NARRATION_ENABLED=true' \
  'SANGUINIUS_TTS_PROVIDER=openai' \
  'SANGUINIUS_APPEARANCES_MODE=dry_run' >"$production_environment"
SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
  "$remote" policy-production-environment "$production_environment"
printf 'SANGUINIUS_TOKEN=forbidden\n' >>"$production_environment"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-production-environment "$production_environment" \
    >/dev/null 2>&1; then
  echo "production environment policy accepted a direct credential" >&2
  exit 1
fi
sed -i '/SANGUINIUS_TOKEN=forbidden/d' "$production_environment"
printf 'SANGUINIUS_TEST_MODE=true\n' >>"$production_environment"
if SANGUINIUS_SCRIPT_TESTING=true SANGUINIUS_TEST_ROOT="$temporary" \
    "$remote" policy-production-environment "$production_environment" \
    >/dev/null 2>&1; then
  echo "production environment policy accepted a duplicate safety setting" >&2
  exit 1
fi

grep -Fq 'systemctl enable sanguinius.service' "$remote"

SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-capacity 1048576 1024
if SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-capacity 1048575 1024 \
    >/dev/null 2>&1; then
  echo "remote capacity policy accepted insufficient disk" >&2
  exit 1
fi

retention=$(SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-retention \
  current previous current previous keep-b keep-a delete-me ../unrelated)
[[ $retention == delete-me ]]
[[ $(SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-failure 16 16 true) == \
   rollback-release ]]
[[ $(SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-failure 13 16 false) == \
   restore-database ]]
[[ $(SANGUINIUS_SCRIPT_TESTING=true "$remote" policy-failure 13 16 true) == \
   preserve-schema ]]

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
