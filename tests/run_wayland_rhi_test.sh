#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -eq 0 ]]; then
    printf 'Usage: %s <test-command> [argument ...]\n' "${0##*/}" >&2
    exit 2
fi

if [[ -z "${XDG_RUNTIME_DIR:-}" || -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'SKIP: no Wayland session is available for the RHI test\n'
    exit 77
fi

wayland_socket="$WAYLAND_DISPLAY"
if [[ "$wayland_socket" != /* ]]; then
    wayland_socket="$XDG_RUNTIME_DIR/$wayland_socket"
fi
if [[ ! -S "$wayland_socket" ]]; then
    printf 'SKIP: Wayland socket %s is not reachable for the RHI test\n' \
        "$wayland_socket"
    exit 77
fi
if command -v nc >/dev/null 2>&1 \
    && ! nc -zU "$wayland_socket" >/dev/null 2>&1; then
    printf 'SKIP: Wayland socket %s cannot be reached from this environment\n' \
        "$wayland_socket"
    exit 77
fi

exec "$@"
