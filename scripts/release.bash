#!/usr/bin/env bash
set -euo pipefail
umask 0022

repository=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
lock_file="$repository/packaging/release-toolchain.lock"
dist="$repository/dist"

usage() {
  echo "Usage: $0 image" >&2
  echo "       $0 package [--revision <commit>] [--release-id <id>] [--schema-target <n>] [--catalog <n>] [--version <version>] [--compat]" >&2
  echo "       $0 verify --archive <archive>" >&2
  exit 2
}

load_lock() {
  [[ -f $lock_file && ! -L $lock_file ]] || exit 1
  # The lock file is source-controlled and permits only assignment lines.
  while IFS='=' read -r name value; do
    [[ $name =~ ^[A-Z][A-Z0-9_]*$ && -n $value ]] || continue
    printf -v "$name" '%s' "$value"
  done <"$lock_file"
  : "${ARCH_IMAGE:?}" "${ARCH_SNAPSHOT:?}" "${DPP_REVISION:?}" \
    "${TOOLCHAIN_ID:?}"
}

require_clean() {
  [[ -z $(git -C "$repository" status --porcelain=v1 --untracked-files=all) ]] || {
    echo "Release packaging requires a clean working tree." >&2
    exit 1
  }
}

image_tag() {
  local digest
  digest=$(
    cd "$repository"
    find packaging/Containerfile packaging/container-build.bash \
      packaging/systemd scripts/release.bash scripts/deploy_nuln.bash \
      scripts/sanguinius-backup.bash scripts/sanguinius-restore.bash \
      scripts/lib/remote_deploy.bash tests/scripts \
      packaging/release-toolchain.lock -type f -print0 | \
      LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | \
      awk '{print substr($1,1,16)}'
  )
  printf 'localhost/sanguinius-release:%s\n' "$digest"
}

remove_temporary_tree() {
  local path=$1
  [[ -n $path && $path == "$dist/"* && -d $path && ! -L $path ]] || return 1
  find -P "$path" -xdev -depth -delete
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
  [[ -f $manifest && ! -L $manifest ]] || return 1
  local hash entry extra relative count=0
  declare -A listed=()
  while read -r hash entry extra; do
    [[ -z $extra && $hash =~ ^[0-9a-f]{64}$ && $entry == ./* ]] || {
      echo "Release manifest contains an invalid entry." >&2
      return 1
    }
    relative=${entry#./}
    [[ $relative != SHARE-MANIFEST.sha256 ]] || return 1
    managed_payload_path "$relative" || {
      echo "Release manifest contains an unmanaged path." >&2
      return 1
    }
    [[ -z ${listed[$relative]+present} ]] || return 1
    listed[$relative]=1
    ((count += 1))
  done <"$manifest"
  (( count > 0 )) || return 1
  while IFS= read -r relative; do
    managed_payload_path "$relative" || {
      echo "Release payload contains an unmanaged path." >&2
      return 1
    }
    [[ $relative == SHARE-MANIFEST.sha256 ||
       -n ${listed[$relative]+present} ]] || {
      echo "Release payload contains an unlisted file." >&2
      return 1
    }
  done < <(find "$release" -type f -printf '%P\n')
  while IFS= read -r relative; do
    managed_payload_directory "$relative" || {
      echo "Release payload contains an unmanaged directory." >&2
      return 1
    }
  done < <(find "$release" -type d -printf '%P\n')
  (cd "$release" && sha256sum --quiet -c SHARE-MANIFEST.sha256)
}

podman_environment() {
  export XDG_RUNTIME_DIR="$dist/.podman-runtime"
  install -d -m 0700 "$XDG_RUNTIME_DIR"
  install -d -m 0700 "$dist/.podman-storage" "$dist/.podman-runroot"
}

podman_cmd() {
  command podman --root "$dist/.podman-storage" \
    --runroot "$dist/.podman-runroot" --storage-driver=vfs "$@"
}

build_image() {
  load_lock
  mkdir -p "$dist"
  podman_environment
  podman_cmd build --pull=always \
    --build-arg "ARCH_IMAGE=$ARCH_IMAGE" \
    --build-arg "ARCH_SNAPSHOT=$ARCH_SNAPSHOT" \
    --build-arg "DPP_REVISION=$DPP_REVISION" \
    --build-arg "GCC_VERSION=$GCC_VERSION" \
    --build-arg "GCC_LIBS_VERSION=$GCC_LIBS_VERSION" \
    --build-arg "GLIBC_VERSION=$GLIBC_VERSION" \
    --build-arg "SQLITE_VERSION=$SQLITE_VERSION" \
    --build-arg "FFMPEG_VERSION=$FFMPEG_VERSION" \
    --build-arg "OPENSSL_VERSION=$OPENSSL_VERSION" \
    --build-arg "CURL_VERSION=$CURL_VERSION" \
    --build-arg "SYSTEMD_VERSION=$SYSTEMD_VERSION" \
    --build-arg "SYSTEMD_LIBS_VERSION=$SYSTEMD_LIBS_VERSION" \
    --build-arg "ZSTD_VERSION=$ZSTD_VERSION" \
    --build-arg "ZLIB_VERSION=$ZLIB_VERSION" \
    --build-arg "ZLIB_NG_VERSION=$ZLIB_NG_VERSION" \
    --build-arg "OPUS_VERSION=$OPUS_VERSION" \
    --build-arg "LIBSODIUM_VERSION=$LIBSODIUM_VERSION" \
    --build-arg "CATCH2_VERSION=$CATCH2_VERSION" \
    --build-arg "CMAKE_VERSION=$CMAKE_VERSION" \
    --build-arg "NINJA_VERSION=$NINJA_VERSION" \
    --build-arg "NLOHMANN_JSON_VERSION=$NLOHMANN_JSON_VERSION" \
    --build-arg "SHELLCHECK_VERSION=$SHELLCHECK_VERSION" \
    --tag "$(image_tag)" \
    --file "$repository/packaging/Containerfile" "$repository"
}

verify_archive() (
  local archive=$1
  [[ $archive == /* && -f $archive && ! -L $archive ]] || {
    echo "Archive path must name a regular absolute path." >&2
    exit 1
  }
  [[ -f $archive.sha256 ]] || {
    echo "External archive checksum is missing." >&2
    exit 1
  }
  local expected actual
  expected=$(awk 'NF == 2 {print $1}' "$archive.sha256")
  actual=$(sha256sum "$archive" | awk '{print $1}')
  [[ $expected =~ ^[0-9a-f]{64}$ && $expected == "$actual" ]] || {
    echo "Archive checksum mismatch." >&2
    exit 1
  }
  local root entry
  root=
  while IFS= read -r entry; do
    [[ -n $entry && $entry != /* && $entry != *'..'* ]] || {
      echo "Archive contains an unsafe path." >&2
      exit 1
    }
    local first=${entry%%/*}
    [[ -n $root ]] || root=$first
    [[ $first == "$root" ]] || {
      echo "Archive is not a single rooted release tree." >&2
      exit 1
    }
  done < <(tar --zstd -tf "$archive")
  [[ $root =~ ^sanguinius-[A-Za-z0-9._+-]+$ ]] || exit 1
  if tar --zstd -tvf "$archive" | awk '$1 !~ /^[-d]/ {found=1} END {exit !found}'; then
    echo "Archive contains an unsupported entry type." >&2
    exit 1
  fi
  local temporary=''
  trap '[[ -z $temporary || ! -d $temporary ]] || remove_temporary_tree "$temporary"' EXIT
  temporary=$(mktemp -d "$dist/verify.XXXXXXXX")
  tar --zstd --extract --file "$archive" --directory "$temporary" \
    --no-same-owner --no-same-permissions
  local release="$temporary/$root"
  [[ -f $release/RELEASE-METADATA.json &&
     -f $release/SHARE-MANIFEST.sha256 &&
     -x $release/bin/sanguinius &&
     -f $release/lib/libdpp.so.10.1.7 ]] || exit 1
  verify_payload_tree "$release"
  if find "$release" -type f -print0 | xargs -0 -r file | \
      grep -Eiq '(private key|sqlite|database|core file)'; then
    echo "Release payload contains a forbidden file type." >&2
    exit 1
  fi
  if find "$release" -type f -printf '%f\n' | \
      grep -Eiq '(^|[._-])(token|secret|credential|openai\.key|bot\.token|messages\.log)($|[._-])'; then
    echo "Release payload contains a forbidden filename." >&2
    exit 1
  fi
  local rpath
  rpath=$(readelf -d "$release/bin/sanguinius" | \
    sed -n 's/.*\(RUNPATH\|RPATH\).*\[\(.*\)\].*/\2/p')
  [[ $rpath == "\$ORIGIN/../lib" ]] || {
    echo "Release RPATH is not production-safe." >&2
    exit 1
  }
  LD_LIBRARY_PATH="$release/lib" ldd "$release/bin/sanguinius" | \
    grep -q 'libdpp.so.10.1.7'
  if LD_LIBRARY_PATH="$release/lib" ldd "$release/bin/sanguinius" | \
      grep -q 'not found'; then
    echo "Release has a missing dynamic dependency." >&2
    exit 1
  fi
  if readelf -n "$release/bin/sanguinius" \
      "$release/lib/libdpp.so.10.1.7" | grep -q 'x86-64-v4'; then
    echo "Release exceeds the x86-64-v3 ISA ceiling." >&2
    exit 1
  fi
  echo "release_archive=verified"
  echo "sha256=$actual"
)

package_release() (
  load_lock
  require_clean
  local revision=HEAD release_id='' schema_target=16 catalog=16 version=2.2.0
  local compat=false
  while (( $# )); do
    case "$1" in
      --revision) (( $# >= 2 )) || usage; revision=$2; shift 2 ;;
      --release-id) (( $# >= 2 )) || usage; release_id=$2; shift 2 ;;
      --schema-target) (( $# >= 2 )) || usage; schema_target=$2; shift 2 ;;
      --catalog) (( $# >= 2 )) || usage; catalog=$2; shift 2 ;;
      --version) (( $# >= 2 )) || usage; version=$2; shift 2 ;;
      --compat) compat=true; shift ;;
      *) usage ;;
    esac
  done
  revision=$(git -C "$repository" rev-parse --verify "$revision^{commit}")
  [[ $revision =~ ^[0-9a-f]{40}$ && $schema_target =~ ^[0-9]+$ &&
     $catalog =~ ^[0-9]+$ && $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || exit 1
  [[ -n $release_id ]] || release_id="$version-g${revision:0:12}"
  [[ $release_id =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ ]] || exit 1
  mkdir -p "$dist"
  local archive="$dist/sanguinius-$release_id.tar.zst"
  local evidence="$dist/sanguinius-$release_id.test-evidence.json"
  [[ ! -e $archive && ! -e $archive.sha256 && ! -e $evidence ]] || {
    echo "Refusing to overwrite an existing release archive." >&2
    exit 1
  }

  local work='' output='' stage release epoch timestamp package_succeeded=false
  # Invoked indirectly by the EXIT trap.
  # shellcheck disable=SC2329
  cleanup_package() {
    if [[ -n $work && -d $work ]]; then
      git -C "$repository" worktree remove --force "$work" >/dev/null 2>&1 || true
    fi
    [[ -z $output || ! -d $output ]] || remove_temporary_tree "$output"
    if [[ $package_succeeded == false ]]; then
      rm -f -- "$archive" "$archive.sha256" "$evidence"
    fi
  }
  trap cleanup_package EXIT
  work=$(mktemp -d "$dist/source.XXXXXXXX")
  output=$(mktemp -d "$dist/output.XXXXXXXX")
  git -C "$repository" worktree add --detach "$work" "$revision" >/dev/null
  epoch=$(git -C "$repository" show -s --format=%ct "$revision")
  timestamp=$(date -u -d "@$epoch" +%Y-%m-%dT%H:%M:%SZ)
  podman_environment
  podman_cmd image exists "$(image_tag)" || {
    echo "Pinned release image is absent; run '$0 image'." >&2
    exit 1
  }
  podman_cmd run --rm --network=none --userns=keep-id \
    --volume "$work:/src:ro" --volume "$output:/out:rw" \
    --env "SOURCE_DATE_EPOCH=$epoch" \
    --env "SANGUINIUS_RELEASE_ID=$release_id" \
    --env "SANGUINIUS_REVISION=$revision" \
    --env "SANGUINIUS_BUILD_TIMESTAMP=$timestamp" \
    --env "SANGUINIUS_TOOLCHAIN_ID=$TOOLCHAIN_ID" \
    --env "SANGUINIUS_COMPATIBILITY_RELEASE=$compat" \
    "$(image_tag)"
  stage="$output/stage"
  release="$output/sanguinius-$release_id"
  mv -T "$stage" "$release"
  install -d -m 0755 "$release/assets/vox" "$release/config" \
    "$release/migrations" "$release/systemd" "$release/sysusers.d" \
    "$release/tmpfiles.d" "$release/docs"
  if [[ -d $release/share/sanguinius/vox ]]; then
    cp -a "$release/share/sanguinius/vox/." "$release/assets/vox/"
    find "$release/share/sanguinius/vox" -type f -delete
    find "$release/share" -depth -type d -empty -delete
  fi
  cp "$work/config/persona.txt" "$work/config/appearance-policy-v2.json" \
    "$work/config/emperor-tarot-v1.json" "$work/config/tarot-house-v1.json" \
    "$release/config/"
  find "$work/migrations" -maxdepth 1 -type f -name '*.sql' \
    ! -name '0017*' -exec cp '{}' "$release/migrations/" ';'
  cp "$repository/packaging/systemd/sanguinius.service" \
    "$repository/packaging/systemd/sanguinius-compat.service" \
    "$repository/packaging/systemd/sanguinius-backup.service" \
    "$repository/packaging/systemd/sanguinius-backup.timer" "$release/systemd/"
  cp "$repository/packaging/sysusers.d/sanguinius.conf" "$release/sysusers.d/"
  cp "$repository/packaging/tmpfiles.d/sanguinius.conf" "$release/tmpfiles.d/"
  cp "$repository/docs/OPERATIONS.md" "$release/docs/OPERATIONS.md"
  if [[ $compat == false ]]; then
    install -D -m 0755 "$repository/scripts/sanguinius-backup.bash" \
      "$release/libexec/sanguinius-backup.bash"
    install -D -m 0755 "$repository/scripts/sanguinius-restore.bash" \
      "$release/libexec/sanguinius-restore.bash"
  fi
  local lock_sha unit archive_sha manifest
  lock_sha=$(sha256sum "$lock_file" | awk '{print $1}')
  unit=sanguinius.service
  [[ $compat == false ]] || unit=sanguinius-compat.service
  printf '{"format_version":1,"release_id":"%s","version":"%s","revision":"%s","build_timestamp":"%s","source_date_epoch":%s,"toolchain":"%s","toolchain_lock_sha256":"%s","schema_target":%s,"command_catalog_version":%s,"service_unit":"%s","compatibility_release":%s}\n' \
    "$release_id" "$version" "$revision" "$timestamp" "$epoch" \
    "$TOOLCHAIN_ID" "$lock_sha" "$schema_target" "$catalog" "$unit" \
    "$compat" >"$release/RELEASE-METADATA.json"
  find "$release" -type l -print -quit | grep -q . && {
    echo "Release staging contains an unexpected symlink." >&2
    exit 1
  }
  manifest="$output/SHARE-MANIFEST.sha256"
  (cd "$release" && find . -type f -print0 | LC_ALL=C sort -z | \
    xargs -0 sha256sum >"$manifest")
  mv -T "$manifest" "$release/SHARE-MANIFEST.sha256"
  find "$release" -exec touch -h -d "@$epoch" '{}' +
  find "$release" -type d -exec chmod 0755 '{}' +
  find "$release" -type f -exec chmod 0644 '{}' +
  chmod 0755 "$release/bin/sanguinius" "$release/lib/libdpp.so.10.1.7"
  [[ ! -d $release/libexec ]] || chmod 0755 "$release/libexec"/*
  tar --sort=name --format=posix \
    --pax-option=delete=atime,delete=ctime \
    --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    --directory "$output" --create --file - "sanguinius-$release_id" | \
    zstd -q -19 --threads=1 -o "$archive"
  printf '%s  %s\n' "$(sha256sum "$archive" | awk '{print $1}')" \
    "$(basename "$archive")" >"$archive.sha256"
  archive_sha=$(sha256sum "$archive" | awk '{print $1}')
  printf '{"format_version":1,"release_id":"%s","revision":"%s","toolchain":"%s","archive_sha256":"%s","container_release_build":"passed","container_ctest":"passed"}\n' \
    "$release_id" "$revision" "$TOOLCHAIN_ID" "$archive_sha" >"$evidence"
  chmod 0444 "$archive" "$archive.sha256" "$evidence"
  verify_archive "$archive"
  package_succeeded=true
  echo "archive=$archive"
)

command=${1:-}
shift || true
case "$command" in
  image) (( $# == 0 )) || usage; build_image ;;
  package) package_release "$@" ;;
  policy-payload-path)
    [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true && $# -eq 1 ]] || usage
    managed_payload_path "$1"
    ;;
  policy-payload-directory)
    [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true && $# -eq 1 ]] || usage
    managed_payload_directory "$1"
    ;;
  policy-payload-tree)
    [[ ${SANGUINIUS_SCRIPT_TESTING:-false} == true && $# -eq 1 ]] || usage
    verify_payload_tree "$1"
    ;;
  verify)
    [[ ${1:-} == --archive && $# -eq 2 ]] || usage
    verify_archive "$2"
    ;;
  *) usage ;;
esac
