#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
clean_build=false
configurations=(debug release)

usage() {
    echo "Usage: $0 [--clean] [debug|release|all]" >&2
}

for argument in "$@"; do
    case "${argument}" in
        --clean)
            clean_build=true
            ;;
        debug|release)
            configurations=("${argument}")
            ;;
        all)
            configurations=(debug release)
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

for configuration in "${configurations[@]}"; do
    build_arguments=("${configuration}")
    if [[ "${clean_build}" == true ]]; then
        build_arguments=(--clean "${configuration}")
    fi
    "${project_root}/scripts/build_bot.bash" "${build_arguments[@]}"
    ctest --preset "${configuration}"
done
