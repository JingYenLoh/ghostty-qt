#!/usr/bin/env bash

set -euo pipefail

usage()
{
    printf 'Usage: %s [opengl|vulkan] [legacy|retained] [source-dirty|effect-only] [0|1|2|4|8]\n' \
        "${0##*/}" >&2
    exit 2
}

graphics_api="${1:-vulkan}"
renderer="${2:-retained}"
workload="${3:-effect-only}"
passes="${4:-8}"
if [[ "$#" -gt 4 ]]; then
    usage
fi

case "$graphics_api" in
    opengl | vulkan) ;;
    *) usage ;;
esac
case "$renderer" in
    legacy | retained) ;;
    *) usage ;;
esac
case "$workload" in
    source-dirty | effect-only) ;;
    *) usage ;;
esac
case "$passes" in
    0 | 1 | 2 | 4 | 8) ;;
    *) usage ;;
esac

if ! command -v renderdoccmd > /dev/null 2>&1; then
    printf 'error: renderdoccmd is required; install RenderDoc first\n' >&2
    exit 1
fi

script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$script_directory/.." && pwd)"
cd "$root"

cmake --preset release -DGHOSTTY_QT_BUILD_RENDER_BENCHMARKS=ON
cmake --build --preset release \
    --target bench-terminal-custom-shader-rhi \
    -j"$(nproc)"

benchmark="$root/build/release/tests/bench-terminal-custom-shader-rhi"
if [[ ! -x "$benchmark" ]]; then
    printf 'error: benchmark was not created at %s\n' "$benchmark" >&2
    exit 1
fi

if [[ -z "${QT_QPA_PLATFORM:-}" ]]; then
    if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
        export QT_QPA_PLATFORM=wayland
    else
        printf 'error: a Wayland session is required for RenderDoc capture\n' >&2
        exit 1
    fi
fi
if [[ "$QT_QPA_PLATFORM" != wayland ]]; then
    printf 'error: QT_QPA_PLATFORM must be wayland, got %s\n' \
        "$QT_QPA_PLATFORM" >&2
    exit 1
fi

capture_directory="$root/tmp/renderdoc"
mkdir -p "$capture_directory"
capture_template="$capture_directory/custom-shader-${graphics_api}-${renderer}-${workload}-${passes}-$(date +%Y%m%d-%H%M%S-%N)"
capture_log="${capture_template}.log"
selector="${renderer}:${workload}:${passes}"

printf 'Capturing %s with template %s\n' "$selector" "$capture_template"
renderdoccmd capture \
    --wait-for-exit \
    --working-dir "$root" \
    --capture-file "$capture_template" \
    "$benchmark" \
    --graphics-api "$graphics_api" \
    --renderdoc-capture "$selector" \
    --renderdoc-capture-path "$capture_template" 2>&1 | tee "$capture_log"

# renderdoccmd --wait-for-exit does not propagate the target's exit status.
# Require the benchmark's success record as well as a non-empty capture so a
# partial capture saved during error unwinding cannot be reported as success.
if ! grep -Fq "renderdoc_capture=${selector} " "$capture_log"; then
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
