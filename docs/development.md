# Development and CI

The checked-in CMake presets are the supported entry points. All builds require
C++23 and use the project-local Zig executable at `.local/bin/zig`.

| Preset | Toolchain | Purpose |
| --- | --- | --- |
| `dev` | Default compiler, Debug | Normal development and tests |
| `release` | Default compiler, Release | Optimized application and tests; opt-in benchmarks |
| `sanitize` | Clang, Debug | AddressSanitizer and UndefinedBehaviorSanitizer |

## Bootstrap

Initialize the pinned Ghostty checkout and install the required Zig version:

```sh
git submodule update --init --recursive
./scripts/bootstrap-zig.sh
./.local/bin/zig version
```

The bootstrap script supports x86-64 and AArch64 Linux. It downloads the exact
version required by the project, verifies the published SHA-256, and installs
under the ignored `.local/toolchains` directory. Archives are cached under
`.cache/zig`.

`ZIG_DOWNLOAD_CACHE` can select another cache directory and
`ZIG_DOWNLOAD_BASE_URL` can select an HTTPS mirror. The expected checksum
cannot be overridden.

## Build and test

Configure, build, and test the developer preset:

```sh
cmake --preset dev
cmake --build --preset dev -j"$(nproc)"
ctest --preset dev -j"$(nproc)" --output-on-failure
```

Use all available processors for every CMake build. Replace `dev` with
`release` or `sanitize` as needed.

List tests or run a focused subset with:

```sh
ctest --preset dev --show-only
ctest --preset dev -j"$(nproc)" --output-on-failure \
    -R 'session-worker|terminal-pane|terminal-workspace'
```

CTest is the maintained test inventory. Broadly, the suite covers:

- libghostty adaptation and PTY lifecycle;
- input, rendering, search, links, clipboard, and Kitty graphics;
- tabs, splits, windows, keybindings, and application lifetime;
- shared and frontend configuration services;
- D-Bus, portal, systemd, cgroup, and desktop integration;
- staged installation, CLI delegation, and parity checks.

Tests isolate configuration homes and external protocol services where
possible. Full Wayland RHI tests require a reachable compositor; CI supplies a
headless Weston instance rather than using X11.

For a diagnostic startup without a Wayland compositor, the application has an
explicitly unsupported offscreen escape hatch:

```sh
GHOSTTY_QT_ALLOW_NON_WAYLAND=1 \
QT_QPA_PLATFORM=offscreen \
QT_QUICK_BACKEND=software \
timeout 3s ./build/dev/ghostty-qt --hold -e /bin/sh -c 'printf "smoke\n"'
```

Exit status 124 is expected because `--hold` keeps the process alive until
`timeout` stops it. This checks startup only; it does not qualify native
Wayland behavior or the hardware renderer.

## Sanitizers

Configure and build once, then use the sanitizer preset normally:

```sh
cmake --preset sanitize
cmake --build --preset sanitize -j"$(nproc)"
ctest --preset sanitize -j"$(nproc)" --output-on-failure
```

The preset enables ASan and UBSan for project C/C++ targets and uses Ghostty's
`ReleaseSafe` Zig mode. Zig archives and the private configuration helper are
not Clang-instrumented, although failures may still surface at an instrumented
C++ boundary.

LeakSanitizer requires process-tracing support. In a managed environment that
prevents its `ptrace` use, the test preset's own environment would override a
shell assignment. Run the already-built suite directly instead:

```sh
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/sanitize --output-on-failure
```

Disabling leak detection is only an environment workaround; keep it enabled on
ordinary hosts.

## Formatting

The repository uses `.clang-format`; version 18 or newer is recommended.
Format changed lines with:

```sh
./scripts/check-format.sh --fix
```

Enable the tracked pre-commit hook once per checkout:

```sh
git config core.hooksPath .githooks
```

The hook checks staged changed C/C++ lines without modifying the worktree.
Other useful modes are:

```sh
./scripts/check-format.sh --staged
./scripts/check-format.sh --diff
```

Review and stage formatting changes again after `--fix`, because it deliberately
does not alter the index.

For Python maintenance, use Ruff rather than Black. The repository does not
currently define a repo-wide automated Ruff gate, so do not document or assume
one until its configuration is checked in.

## Performance work

Performance targets are opt-in and excluded from CTest. Their build commands,
benchmark selection, renderer qualification, RenderDoc capture, comparison
policy, and measured optimization backlog are in
[Performance](performance.md).

## Configuration-helper boundary

The complete Ghostty application parser is not exported by `libghostty-vt`.
When `GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG=ON` (the default), CMake builds a private
revision-matched runtime used only by `ghostty-qt-config-helper`. The main
application consumes a strict typed projection through a child-process
protocol.

The same helper is the process-replacement target for supported pinned `+`
actions. It also supplies Fontconfig-backed font commands, themes, SSH
wrappers, and shell-integration finalization. The official Ghostty submodule is
not patched; project overlays and source shadows are generated in ignored
cache directories.

To work on the Qt application without the private parser, configure with:

```sh
cmake --preset dev -DGHOSTTY_QT_ENABLE_GHOSTTY_CONFIG=OFF
```

This is a development configuration, not the normal feature set.

Generated caches live below `.cache`. Ghostty's main Zig wrapper also writes
to `ghostty/zig-out`, which is shared by all CMake presets. Sequential preset
switches rebuild incompatible outputs automatically; concurrent preset builds
in one checkout are unsupported.

## Parity and upstream upgrades

Check the pinned inventory without compiling:

```sh
python3 scripts/check-ghostty-parity.py
```

The checker verifies that `GHOSTTY_REVISION`, the submodule checkout, CMake,
and `docs/ghostty-parity.json` agree, then derives configuration and action
inventories from upstream declarations.

Shell completions are generated from the frontend-owned schema in
`dist/shell-completions/spec.json`:

```sh
python3 scripts/generate-shell-completions.py
python3 scripts/generate-shell-completions.py --check
```

The generator emits config-enabled and config-disabled Bash, Fish, and Zsh
artifacts. Tests keep the ordinary options aligned with the Qt parser and the
action names/options aligned with the pinned Ghostty inventory; the installed
scripts never import Ghostty's private completion generator.

For an intentional Ghostty upgrade:

1. update the submodule and `GHOSTTY_REVISION` together;
2. inspect public C API, private helper/schema, required Zig version, and
   bootstrap checksums;
3. update project overlays, inventory extraction, the parity manifest, and the
   shell completion schema where required;
4. run the parity checker and focused adapter/configuration tests;
5. qualify complete Debug, Release, and sanitizer builds;
6. keep the upstream submodule free of local commits.

Do not implement a missing public terminal contract by inspecting PTY bytes or
copying Ghostty internals into the frontend. Record such work in
[`REQUIRES_UPSTREAM.md`](../REQUIRES_UPSTREAM.md).

## Continuous integration

GitHub Actions currently covers:

- GCC Debug and Release against the minimum Qt 6.10 line;
- Clang ASan+UBSan against the minimum Qt line;
- GCC Debug against the current Qt line;
- GCC Debug with the private Ghostty configuration parser disabled.

Each job uses an independent checkout and verifies Qt, Zig, and Ghostty
revisions before configuring. A private headless Weston compositor is used for
required OpenGL/Wayland tests. The workflow file is the authoritative version
matrix.
