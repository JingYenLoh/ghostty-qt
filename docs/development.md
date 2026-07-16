# Development and CI

The checked-in CMake presets are the supported developer entry points. Each
preset uses the project-local Zig executable at `.local/bin/zig` and builds in
its own directory:

| Preset | C++ toolchain | Purpose |
| --- | --- | --- |
| `dev` | Default compiler, Debug | Normal development and tests |
| `release` | Default compiler, Release | Optimized build and release-mode tests |
| `sanitize` | Clang, Debug | AddressSanitizer and UndefinedBehaviorSanitizer |

Bootstrap the exact Zig version required by the pinned Ghostty revision with:

```sh
./scripts/bootstrap-zig.sh
```

The script supports x86-64 and AArch64 Linux. It downloads Zig 0.15.2 from the
official Zig release directory, verifies the architecture-specific SHA-256
published in Zig's release index, extracts it under `.local/toolchains`, and
creates `.local/bin/zig`. A downloaded archive is cached under `.cache/zig` and
is verified again before extraction. `ZIG_DOWNLOAD_CACHE` can select another
cache directory and `ZIG_DOWNLOAD_BASE_URL` can select an HTTPS mirror; the
expected checksum cannot be overridden.

Configure, build, and test a preset as one sequence:

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

The suite includes focused contracts for the libghostty adapter, workspace
identity/action foundation, Ghostty action catalog, dirty-update transport,
typed config overlays, watched reload, flattened keybinding matching, and the
config-helper process protocol,
as well as PTY, renderer, application-lifecycle, parity, exact-parser smoke, and
relocatable-install coverage. List or run individual tests with:

```sh
ctest --preset dev --show-only
ctest --preset dev -R 'ghostty-vt-adapter|workspace-foundation|terminal-workspace|ghostty-action-catalog|ghostty-keybind-set|ghostty-config|ghostty-parity-manifest'
```

## Ghostty configuration parser

Configuration is enabled by default with
`GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG=ON`. Because the exact Ghostty application
parser is outside `libghostty-vt`, the build produces a private
`ghostty-internal` shared library and links it only into
`ghostty-qt-config-helper`. The main process talks to that helper through
Ghostty's `+validate-config` and `+show-config` CLI actions and converts only the
documented compatibility keys into value snapshots.

The additional Zig outputs and global package/artifact cache live under:

```text
.cache/ghostty-internal/<ghostty-revision>/
.cache/zig-global/
```

Both paths are project-local and ignored by Git. They are shared by the CMake
presets, so do not configure or build presets concurrently. A cold config-parser
build is much larger and slower than an ordinary incremental C++ rebuild. To
work on the application without this integration, configure manually with
`-DGHOSTTY_QT_ENABLE_GHOSTTY_CONFIG=OFF`; that is a development option, not the
normal feature set.

The three focused config tests have distinct boundaries:

- `ghostty-config-service` exercises filesystem discovery/watch/debounce,
  missing optional includes, generation-safe asynchronous reload, and last-good
  snapshot behavior with an injected loader.
- `ghostty-config-process-loader` uses a fake helper to make protocol ordering,
  post-query validation, canonical output parsing, warnings, timeouts, crashes,
  and failures deterministic; one case joins the loader to the real helper to
  verify Ghostty's effective `clear`/`unbind` result.
- `ghostty-config-helper-smoke` runs the actual helper against the exact pinned
  parser with an isolated `XDG_CONFIG_HOME`.

The app lifecycle test also uses an isolated config home, so it never reads a
developer's real Ghostty configuration.

`ghostty-keybind-set` covers the current local single-key compatibility layer,
including Linux native physical locations and shifted punctuation lookup.
The helper's formatted output does not preserve Ghostty's binding flags, so
sequences, tables, global/all-surface behavior, and exact flag semantics must
remain explicit later stages rather than being emulated with Qt shortcuts.

## Ghostty parity manifest

`docs/ghostty-parity.json` is the machine-checked coverage ledger for the
pinned Ghostty snapshot. It records the Linux/Wayland/Qt scope and tracks every
upstream configuration key, keybinding action, and CLI action as supported,
partial, planned, blocked upstream, or not applicable. A planned entry means
only that it is in scope; it does not claim the feature is implemented.

Run the check without compiling the application:

```sh
python3 scripts/check-ghostty-parity.py
```

The checker requires the pinned Ghostty submodule. It verifies that the
manifest, checkout, and CMake pin name the same revision, then derives fresh
inventories from the upstream Zig declarations and reports deterministic drift.
CTest runs the same contract as `ghostty-parity-manifest`. Update the pinned
revision, extractor (if upstream declarations changed shape), and manifest in
one reviewed change when intentionally advancing the Ghostty snapshot.

For a sanitizer run, install Clang and use:

```sh
cmake --preset sanitize
cmake --build --preset sanitize --target clean
cmake --build --preset sanitize --parallel
ctest --preset sanitize
```

LeakSanitizer itself requires ptrace support. In a restricted container that
denies ptrace, keep ASan/UBSan enabled but disable only leak detection and avoid
the test preset's overriding environment:

```sh
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/sanitize --output-on-failure
```

The sanitizer preset instruments the project's C and C++ targets with ASan and
UBSan, enables frame pointers, and stops on undefined behavior. It also changes
the pinned Ghostty build from `ReleaseFast` to `ReleaseSafe`, retaining Zig's
safety checks. The Ghostty static library is not ASan/UBSan-instrumented. A
failure in upstream Zig code can still be visible at an instrumented API
boundary, but this preset does not provide complete sanitizer coverage inside
that library. The private config-parser build remains `ReleaseFast` and is also
not sanitizer-instrumented; its process boundary is intentional containment,
not a replacement for upstream testing.

The clean step is required when another preset has already populated the shared
`ghostty/zig-out` directory. Ghostty's CMake wrapper does not encode Zig
optimization flags in that output path, so an existing `ReleaseFast` archive
would otherwise satisfy the sanitizer build without being regenerated as
`ReleaseSafe`. Likewise, clean the destination preset before switching back to
a different Zig configuration. This is another reason never to build the
presets concurrently in one checkout.

## Continuous integration

GitHub Actions runs Debug, Release, and Clang ASan+UBSan against the minimum Qt
6.8 line, currently pinned to Qt 6.8.3. A fourth Debug job checks compatibility
with the current Qt line, pinned to Qt 6.11.1. Every job has its own Ubuntu
24.04 checkout, including the pinned Ghostty submodule, so Ghostty's source-tree
`zig-out` directory is never shared between simultaneous configurations. CI
verifies the Qt and Zig versions and Ghostty commit before configuring, then
runs the matching CMake build and CTest presets.

Do not build multiple presets concurrently in the same checkout. Although the
CMake output directories are separate, the embedded Ghostty build writes to
`ghostty/zig-out`, and the config-parser integration writes to the shared
project-local `.cache` directories.
