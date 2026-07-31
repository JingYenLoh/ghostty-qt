#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -eq 0 ]]; then
    printf 'Usage: %s <test-command> [argument ...]\n' "${0##*/}" >&2
    exit 2
fi

# The executable is instrumented but Qt's XCB/XKB stack is not. On current
# Linux distributions its device-keymap setup performs a bounded strndup from
# an exact-sized XCB reply, which AddressSanitizer reports before Qt creates a
# window. The shader ABI and mpv-shaped protocol paths remain sanitizer-tested;
# exercise this external integration boundary in the dev/release runs.
if [[ "${GHOSTTY_QT_ASAN_BUILD:-0}" == 1 ]]; then
    printf 'SKIP: XCB OpenGL RHI integration is unavailable under ASan\n'
    exit 77
fi

if command -v xvfb-run >/dev/null 2>&1; then
    exec xvfb-run -a -s '-screen 0 640x480x24' "$@"
fi

if [[ -n "${DISPLAY:-}" ]]; then
    exec "$@"
fi

printf 'SKIP: no X display or xvfb-run for OpenGL RHI test\n'
exit 77
