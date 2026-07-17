#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
preset="release"
clean_build=false

usage() {
    echo "Usage: $0 [--clean] [release|debug]" >&2
}

for argument in "$@"; do
    case "${argument}" in
        --clean)
            clean_build=true
            ;;
        release|debug)
            preset="${argument}"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

cd "${project_root}"

echo "Configuring Sanguinius with the '${preset}' CMake preset..."
cmake --preset "${preset}"

build_options=(--build --preset "${preset}" --parallel --verbose)
if [[ "${clean_build}" == true ]]; then
    echo "Clean-building Sanguinius with full compiler output..."
    build_options+=(--clean-first)
else
    echo "Building Sanguinius with full compiler output..."
fi

cmake "${build_options[@]}"
