#!/usr/bin/env bash

set -euo pipefail

usage()
{
    printf 'Usage: %s [opengl|vulkan] [scenario]\n' "${0##*/}" >&2
    printf 'Scenarios: metadata one-dirty-row cursor-only full-invalidation search-update\n' >&2
    printf '           kitty-first-upload kitty-retained-redraw kitty-movement\n' >&2
    printf '           kitty-replacement kitty-eviction\n' >&2
    exit 2
}

graphics_api="${1:-vulkan}"
scenario="${2:-kitty-movement}"
if [[ "$#" -gt 2 ]]; then
    usage
fi

case "$graphics_api" in
    opengl | vulkan) ;;
    *) usage ;;
esac
case "$scenario" in
    metadata | one-dirty-row | cursor-only | full-invalidation | search-update \
        | kitty-first-upload | kitty-retained-redraw | kitty-movement \
        | kitty-replacement | kitty-eviction) ;;
    *) usage ;;
esac

if ! command -v renderdoccmd >/dev/null 2>&1; then
    printf 'error: renderdoccmd is required; install RenderDoc first\n' >&2
    exit 1
fi

script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$script_directory/.." && pwd)"
cd "$root"

cmake --preset release -DGHOSTTY_QT_BUILD_RENDER_BENCHMARKS=ON
cmake --build --preset release \
    --target bench-terminal-pane-renderer \
    -j"$(nproc)"

benchmark="$root/build/release/tests/bench-terminal-pane-renderer"
if [[ ! -x "$benchmark" ]]; then
    printf 'error: benchmark was not created at %s\n' "$benchmark" >&2
    exit 1
fi

if [[ -z "${QT_QPA_PLATFORM:-}" ]]; then
    if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
        export QT_QPA_PLATFORM=wayland
    elif [[ -n "${DISPLAY:-}" ]]; then
        export QT_QPA_PLATFORM=xcb
    else
        printf 'warning: no graphical session detected; trying Qt offscreen, which may not initialize this RHI\n' >&2
        export QT_QPA_PLATFORM=offscreen
    fi
fi

capture_directory="$root/tmp/renderdoc"
mkdir -p "$capture_directory"
capture_template="$capture_directory/terminal-pane-${graphics_api}-${scenario}-$(date +%Y%m%d-%H%M%S-%N)"
capture_log="${capture_template}.log"

printf 'Capturing %s with %s using template %s\n' \
    "$scenario" "$graphics_api" "$capture_template"
renderdoccmd capture \
    --wait-for-exit \
    --working-dir "$root" \
    --capture-file "$capture_template" \
    "$benchmark" \
    --graphics-api "$graphics_api" \
    --warmup 200 \
    --iterations 1 \
    --renderdoc-capture "$scenario" \
    --renderdoc-capture-path "$capture_template" 2>&1 | tee "$capture_log"

# renderdoccmd --wait-for-exit does not propagate the target's exit status.
# Require the benchmark's post-capture success record and a non-empty capture.
if ! grep -Fq "renderdoc_capture_complete=${scenario}" "$capture_log"; then
    printf 'error: benchmark did not report a successful capture; see %s\n' \
        "$capture_log" >&2
    exit 1
fi

shopt -s nullglob
captures=("${capture_template}"*.rdc)
valid_capture=
for capture in "${captures[@]}"; do
    if [[ -s "$capture" ]]; then
        valid_capture="$capture"
        break
    fi
done
if [[ -z "$valid_capture" ]]; then
    printf 'error: RenderDoc exited without writing a non-empty %s*.rdc\n' \
        "$capture_template" >&2
    exit 1
fi
printf 'Capture written: %s\n' "$valid_capture"
