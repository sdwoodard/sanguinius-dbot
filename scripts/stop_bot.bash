#!/usr/bin/env bash
set -euo pipefail
umask 077

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
pid_file="${project_root}/run/sanguinius.pid"

if [[ ! -f "${pid_file}" ]]; then
    echo "Sanguinius is not running (no PID file)."
    exit 0
fi

bot_pid="$(<"${pid_file}")"
if ! [[ "${bot_pid}" =~ ^[0-9]+$ ]]; then
    echo "Invalid PID file: ${pid_file}" >&2
    exit 1
fi

if kill -0 "${bot_pid}" 2>/dev/null; then
    kill "${bot_pid}"
    for _ in {1..100}; do
        if ! kill -0 "${bot_pid}" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
fi

if kill -0 "${bot_pid}" 2>/dev/null; then
    echo "Sanguinius did not stop within 10 seconds (PID ${bot_pid})." >&2
    exit 1
fi

rm -f "${pid_file}"
echo "Sanguinius is offline."
