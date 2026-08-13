#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -eq 0 ]]; then
    printf 'Usage: %s <test-command> [argument ...]\n' "${0##*/}" >&2
    exit 2
fi

unavailable()
{
    if [[ "${GHOSTTY_QT_REQUIRE_WAYLAND_RHI:-0}" == 1 ]]; then
        printf 'ERROR: %s\n' "$1" >&2
        exit 1
    fi
    printf 'SKIP: %s\n' "$1"
    exit 77
}

if [[ -z "${XDG_RUNTIME_DIR:-}" || -z "${WAYLAND_DISPLAY:-}" ]]; then
    unavailable 'no Wayland session is available for the RHI test'
fi

wayland_socket="$WAYLAND_DISPLAY"
if [[ "$wayland_socket" != /* ]]; then
    wayland_socket="$XDG_RUNTIME_DIR/$wayland_socket"
fi
if [[ ! -S "$wayland_socket" ]]; then
    unavailable "Wayland socket $wayland_socket is not available"
fi
if command -v nc > /dev/null 2>&1 &&
    ! nc -zU "$wayland_socket" > /dev/null 2>&1; then
    unavailable \
        "Wayland socket $wayland_socket cannot be reached from this environment"
fi

exec "$@"
