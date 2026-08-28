#!/usr/bin/env bash
set -euo pipefail

repository=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
release_tool="$repository/scripts/release.bash"
temporary=$(mktemp -d /tmp/sanguinius-release-test.XXXXXXXX)
cleanup() {
  find "$temporary" -type f -delete
  find "$temporary" -type l -delete
  find "$temporary" -depth -type d -empty -delete
}
trap cleanup EXIT

for script in "$release_tool" "$repository/packaging/container-build.bash"; do
  bash -n "$script"
done

for path in bin/sanguinius lib/libdpp.so.10.1.7 \
            config/persona.txt migrations/0016_cross_feature_reliability.sql \
            assets/vox/entrance.pcm docs/OPERATIONS.md; do
  SANGUINIUS_SCRIPT_TESTING=true "$release_tool" policy-payload-path "$path"
done
for path in CMakeCache.txt CMakeFiles/object.o build.ninja cache/tts.pcm \
            state.sqlite3 messages.log config/bot.token; do
  if SANGUINIUS_SCRIPT_TESTING=true "$release_tool" policy-payload-path \
      "$path" >/dev/null 2>&1; then
    echo "release payload policy accepted unmanaged content: $path" >&2
    exit 1
  fi
done

for directory in '' bin lib config assets assets/vox migrations libexec \
                 systemd sysusers.d tmpfiles.d docs; do
  SANGUINIUS_SCRIPT_TESTING=true "$release_tool" \
    policy-payload-directory "$directory"
done
if SANGUINIUS_SCRIPT_TESTING=true "$release_tool" \
    policy-payload-directory cache >/dev/null 2>&1; then
  echo "release payload policy accepted an unmanaged directory" >&2
  exit 1
fi

payload="$temporary/payload"
mkdir "$payload"
printf '{}\n' >"$payload/RELEASE-METADATA.json"
(cd "$payload" && sha256sum ./RELEASE-METADATA.json \
  >SHARE-MANIFEST.sha256)
SANGUINIUS_SCRIPT_TESTING=true "$release_tool" policy-payload-tree "$payload"
mkdir "$payload/docs"
printf 'unlisted\n' >"$payload/docs/OPERATIONS.md"
if SANGUINIUS_SCRIPT_TESTING=true "$release_tool" policy-payload-tree \
    "$payload" >/dev/null 2>&1; then
  echo "release payload policy accepted an unlisted file" >&2
  exit 1
fi
rm -f "$payload/docs/OPERATIONS.md"
rmdir "$payload/docs"
mkdir "$payload/cache"
if SANGUINIUS_SCRIPT_TESTING=true "$release_tool" policy-payload-tree \
    "$payload" >/dev/null 2>&1; then
  echo "release payload policy accepted an unmanaged empty directory" >&2
  exit 1
fi
rmdir "$payload/cache"

printf 'not an archive\n' >"$temporary/corrupt.tar.zst"
printf '%064d  corrupt.tar.zst\n' 0 >"$temporary/corrupt.tar.zst.sha256"
if "$release_tool" verify --archive "$temporary/corrupt.tar.zst" \
    >/dev/null 2>&1; then
  echo "release verify accepted a checksum mismatch" >&2
  exit 1
fi

mkdir "$temporary/tree"
printf 'payload\n' >"$temporary/tree/file"
tar -C "$temporary" -cf - tree | zstd -q -T1 -o "$temporary/multi.tar.zst"
sha=$(sha256sum "$temporary/multi.tar.zst" | awk '{print $1}')
printf '%s  multi.tar.zst\n' "$sha" >"$temporary/multi.tar.zst.sha256"
if "$release_tool" verify --archive "$temporary/multi.tar.zst" \
    >/dev/null 2>&1; then
  echo "release verify accepted an invalid release root" >&2
  exit 1
fi

mkdir "$temporary/sanguinius-link-test"
printf 'payload\n' >"$temporary/sanguinius-link-test/file"
ln "$temporary/sanguinius-link-test/file" \
  "$temporary/sanguinius-link-test/hardlink"
tar -C "$temporary" -cf - sanguinius-link-test | \
  zstd -q -T1 -o "$temporary/link.tar.zst"
sha=$(sha256sum "$temporary/link.tar.zst" | awk '{print $1}')
printf '%s  link.tar.zst\n' "$sha" >"$temporary/link.tar.zst.sha256"
if "$release_tool" verify --archive "$temporary/link.tar.zst" \
    >/dev/null 2>&1; then
  echo "release verify accepted an archive hard link" >&2
  exit 1
fi

ln -s file "$temporary/sanguinius-link-test/symlink"
tar -C "$temporary" -cf - sanguinius-link-test | \
  zstd -q -T1 -o "$temporary/symlink.tar.zst"
sha=$(sha256sum "$temporary/symlink.tar.zst" | awk '{print $1}')
printf '%s  symlink.tar.zst\n' "$sha" >"$temporary/symlink.tar.zst.sha256"
if "$release_tool" verify --archive "$temporary/symlink.tar.zst" \
    >/dev/null 2>&1; then
  echo "release verify accepted an archive symlink" >&2
  exit 1
fi

tar -C "$temporary" -cf - \
  --transform='s,^sanguinius-link-test,../sanguinius-traversal,' \
  sanguinius-link-test/file | zstd -q -T1 -o "$temporary/traversal.tar.zst"
sha=$(sha256sum "$temporary/traversal.tar.zst" | awk '{print $1}')
printf '%s  traversal.tar.zst\n' "$sha" \
  >"$temporary/traversal.tar.zst.sha256"
if "$release_tool" verify --archive "$temporary/traversal.tar.zst" \
    >/dev/null 2>&1; then
  echo "release verify accepted archive traversal" >&2
  exit 1
fi

echo "release shell tests passed"
