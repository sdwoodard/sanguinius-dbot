#!/usr/bin/env bash
set -euo pipefail

: "${SANGUINIUS_RELEASE_ID:?release ID is required}"
: "${SANGUINIUS_REVISION:?revision is required}"
: "${SANGUINIUS_BUILD_TIMESTAMP:?build timestamp is required}"
: "${SANGUINIUS_TOOLCHAIN_ID:?toolchain identity is required}"
: "${SANGUINIUS_COMPATIBILITY_RELEASE:?compatibility release flag is required}"
: "${SOURCE_DATE_EPOCH:?source date epoch is required}"

[[ $SANGUINIUS_COMPATIBILITY_RELEASE == true ||
   $SANGUINIUS_COMPATIBILITY_RELEASE == false ]]

test -d /src
test -d /out
test ! -e /out/stage

mapfile -d '' shell_scripts < <(
  find /opt/sanguinius-release-support -type f -name '*.bash' -print0
)
bash -n /usr/local/bin/sanguinius-container-build "${shell_scripts[@]}"
shellcheck -x /usr/local/bin/sanguinius-container-build "${shell_scripts[@]}"

systemd_root=$(mktemp -d /tmp/sanguinius-systemd-verify.XXXXXXXX)
offline_root=''
# Invoked indirectly by the EXIT trap.
# shellcheck disable=SC2329
cleanup_systemd_root() {
  find -P "$systemd_root" -xdev -depth -delete
  [[ -z $offline_root || ! -d $offline_root ]] ||
    find -P "$offline_root" -xdev -depth -delete
}
trap cleanup_systemd_root EXIT
install -d "$systemd_root/etc/systemd/system" \
  "$systemd_root/opt/sanguinius/current/bin" \
  "$systemd_root/opt/sanguinius/current/libexec" \
  "$systemd_root/etc/sanguinius"
install -m 0755 /usr/bin/true \
  "$systemd_root/opt/sanguinius/current/bin/sanguinius"
install -m 0755 /usr/bin/true \
  "$systemd_root/opt/sanguinius/current/libexec/sanguinius-backup.bash"
install -m 0644 /opt/sanguinius-release-support/packaging/systemd/* \
  "$systemd_root/etc/systemd/system/"
touch "$systemd_root/etc/sanguinius/sanguinius.env" \
  "$systemd_root/etc/sanguinius/bot.token" \
  "$systemd_root/etc/sanguinius/openai.key"
for target in basic local-fs multi-user network-online shutdown sysinit timers; do
  printf '[Unit]\nDescription=Verification stub for %s.target\n' "$target" \
    >"$systemd_root/etc/systemd/system/$target.target"
done
systemd_log="$systemd_root/systemd-analyze.log"
if ! systemd-analyze verify --root="$systemd_root" \
    sanguinius.service sanguinius-compat.service \
    sanguinius-backup.service sanguinius-backup.timer 2>"$systemd_log"; then
  cat "$systemd_log" >&2
  exit 1
fi
if grep -Eq 'Unknown key|Unknown lvalue|Failed to parse' "$systemd_log"; then
  cat "$systemd_log" >&2
  exit 1
fi

release_metadata_options=()
if [[ $SANGUINIUS_COMPATIBILITY_RELEASE == false ]]; then
  release_metadata_options=(
    "-DSANGUINIUS_RELEASE_ID=${SANGUINIUS_RELEASE_ID}"
    "-DSANGUINIUS_BUILD_TIMESTAMP=${SANGUINIUS_BUILD_TIMESTAMP}"
    "-DSANGUINIUS_TOOLCHAIN_ID=${SANGUINIUS_TOOLCHAIN_ID}"
  )
fi
cmake -S /src -B /tmp/sanguinius-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/dpp \
  -DCMAKE_INSTALL_PREFIX=/ \
  -DCMAKE_INSTALL_RPATH="\$ORIGIN/../lib" \
  -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=FALSE \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -march=x86-64-v3 -mtune=generic -ffile-prefix-map=/src=. -fdebug-prefix-map=/src=. -isystem /opt/dpp/include" \
  -DSANGUINIUS_REVISION_OVERRIDE="${SANGUINIUS_REVISION}" \
  "${release_metadata_options[@]}"
cmake --build /tmp/sanguinius-build --parallel 2
install -d /tmp/sanguinius-build/config /tmp/sanguinius-build/assets \
  /tmp/sanguinius-build/tests/fixtures
cp -a /src/config/. /tmp/sanguinius-build/config/
cp -a /src/assets/. /tmp/sanguinius-build/assets/
cp -a /src/tests/fixtures/. /tmp/sanguinius-build/tests/fixtures/
ctest --test-dir /tmp/sanguinius-build --output-on-failure --timeout 120

offline_root=$(mktemp -d /tmp/sanguinius-offline-self-check.XXXXXXXX)
install -d -m 0700 "$offline_root/tts"
offline_database="$offline_root/sanguinius.sqlite3"
offline_backup="$offline_root/backup.sqlite3"
offline_archive="$offline_root/backup.sqlite3.zst"
offline_restored="$offline_root/restored.sqlite3"
offline_log="$offline_root/startup.log"
SANGUINIUS_DATABASE_FILE="$offline_database" \
  /tmp/sanguinius-build/sanguinius db migrate
SANGUINIUS_DATABASE_FILE="$offline_database" \
  /tmp/sanguinius-build/sanguinius db check
SANGUINIUS_DATABASE_FILE="$offline_database" \
  /tmp/sanguinius-build/sanguinius db integrity
if [[ $SANGUINIUS_COMPATIBILITY_RELEASE == true ]]; then
  SANGUINIUS_DATABASE_FILE="$offline_database" \
    /tmp/sanguinius-build/sanguinius db relationships check
  SANGUINIUS_DATABASE_FILE="$offline_database" \
    /tmp/sanguinius-build/sanguinius db tarot check
else
  SANGUINIUS_DATABASE_FILE="$offline_database" \
    /tmp/sanguinius-build/sanguinius db invariants check
fi
SANGUINIUS_DATABASE_FILE="$offline_database" \
  /tmp/sanguinius-build/sanguinius db backup "$offline_backup"
zstd -q -19 --threads=1 -o "$offline_archive" "$offline_backup"
zstd -q -t "$offline_archive"
zstd -q -d -o "$offline_restored" "$offline_archive"
SANGUINIUS_DATABASE_FILE="$offline_restored" \
  /tmp/sanguinius-build/sanguinius db migrate
SANGUINIUS_DATABASE_FILE="$offline_restored" \
  /tmp/sanguinius-build/sanguinius db check

offline_environment=(
  SANGUINIUS_TOKEN=SANGUINIUS_OFFLINE_DISCORD_SENTINEL
  OPENAI_API_KEY=SANGUINIUS_OFFLINE_OPENAI_SENTINEL
  SANGUINIUS_GUILD_ID=10
  SANGUINIUS_PRIMARY_CHANNEL_ID=20
  SANGUINIUS_OWNER_USER_ID=30
  SANGUINIUS_DATABASE_FILE="$offline_database"
  SANGUINIUS_LOG_FILE="$offline_root/messages.log"
  SANGUINIUS_TTS_CACHE_DIRECTORY="$offline_root/tts"
  SANGUINIUS_PERSONA_FILE=/src/config/persona.txt
  SANGUINIUS_APPEARANCE_POLICY_FILE=/src/config/appearance-policy-v2.json
  SANGUINIUS_OPENAI_INPUT_MICRO_USD_PER_MILLION_TOKENS=1
  SANGUINIUS_OPENAI_OUTPUT_MICRO_USD_PER_MILLION_TOKENS=1
  SANGUINIUS_ADMIN_COMMANDS_ENABLED=false
  SANGUINIUS_TEST_MODE=false
  SANGUINIUS_APPEARANCES_MODE=off
  SANGUINIUS_VOICE_INPUT_ENABLED=false
  SANGUINIUS_VOICE_INPUT_GUILD_CONSENT_ATTESTED=false
  SANGUINIUS_TRANSCRIPTION_PROVIDER=disabled
)
env "${offline_environment[@]}" /tmp/sanguinius-build/sanguinius \
  --check-config >/dev/null
set +e
timeout --signal=TERM --kill-after=10s 5s \
  env "${offline_environment[@]}" /tmp/sanguinius-build/sanguinius \
  >"$offline_log" 2>&1
offline_status=$?
set -e
[[ $offline_status -ne 0 ]] || {
  echo "No-network startup unexpectedly exited successfully." >&2
  exit 1
}
if grep -Eq 'SANGUINIUS_OFFLINE_(DISCORD|OPENAI)_SENTINEL' "$offline_log"; then
  echo "No-network startup exposed a credential sentinel." >&2
  exit 1
fi
SANGUINIUS_DATABASE_FILE="$offline_database" \
  /tmp/sanguinius-build/sanguinius db check

DESTDIR=/out/stage cmake --install /tmp/sanguinius-build
install -D -m 0755 /opt/dpp/lib/libdpp.so.10.1.7 \
  /out/stage/lib/libdpp.so.10.1.7
installed_rpath=$(readelf -d /out/stage/bin/sanguinius | \
  sed -n 's/.*\(RUNPATH\|RPATH\).*\[\(.*\)\].*/\2/p')
if [[ $SANGUINIUS_COMPATIBILITY_RELEASE == true ]]; then
  [[ $installed_rpath == '$ORIGIN/../lib:/opt/dpp/lib' ]]
  cmake -DINPUT_FILE=/out/stage/bin/sanguinius \
    "-DOLD_RPATH=$installed_rpath" \
    '-DNEW_RPATH=$ORIGIN/../lib' \
    -P /opt/sanguinius-release-support/packaging/set-rpath.cmake
fi
[[ $(readelf -d /out/stage/bin/sanguinius | \
  sed -n 's/.*\(RUNPATH\|RPATH\).*\[\(.*\)\].*/\2/p') == \
  '$ORIGIN/../lib' ]]
