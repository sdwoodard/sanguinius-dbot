#!/usr/bin/env bash
set -euo pipefail
umask 077

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${project_root}/build/release/sanguinius"
pid_file="${project_root}/run/sanguinius.pid"
console_log="${project_root}/logs/console.log"

if [[ -z "${SANGUINIUS_TOKEN:-}" && -z "${SANGUINIUS_TOKEN_FILE:-}" ]]; then
    export SANGUINIUS_TOKEN_FILE="${HOME}/.config/sanguinius/bot.token"
fi
if [[ -z "${OPENAI_API_KEY:-}" && -z "${SANGUINIUS_OPENAI_API_KEY_FILE:-}" ]]; then
    export SANGUINIUS_OPENAI_API_KEY_FILE="${HOME}/.config/sanguinius/openai.key"
fi

if [[ ! -x "${binary}" ]]; then
    echo "Release binary not found. Run: ./scripts/build_bot.bash" >&2
    exit 1
fi

cd "${project_root}"
if ! "${binary}" --check-config; then
    echo "Sanguinius configuration check failed; the bot was not started." >&2
    exit 1
fi

if [[ -f "${pid_file}" ]]; then
    existing_pid="$(<"${pid_file}")"
    if [[ "${existing_pid}" =~ ^[0-9]+$ ]] && kill -0 "${existing_pid}" 2>/dev/null; then
        echo "Sanguinius is already running (PID ${existing_pid})."
        exit 0
    fi
fi

mkdir -p "${project_root}/run" "${project_root}/logs"
nohup "${binary}" >>"${console_log}" 2>&1 &
bot_pid=$!
echo "${bot_pid}" >"${pid_file}"

sleep 1
if ! kill -0 "${bot_pid}" 2>/dev/null; then
    rm -f "${pid_file}"
    echo "Sanguinius failed to start; inspect ${console_log}." >&2
    exit 1
fi

echo "Sanguinius is online (PID ${bot_pid})."
