# ghostty-qt

`ghostty-qt` is a Linux, Wayland-only terminal emulator MVP built with Qt Quick,
C++20, and Ghostty's `libghostty-vt` C API. Ghostty supplies terminal parsing,
screen state, selection, and input encoding; Qt supplies the window, controls,
scene-graph rendering, clipboard, and input-method integration.

This is an early developer build, not a drop-in replacement for the Ghostty
application. See [Architecture](docs/architecture.md) for the design and the
current tradeoffs, and [Feasibility and stack decision](docs/feasibility.md)
for the host-language comparison and remaining engineering risks.

## What works

- A real Linux PTY and a shell or command per pane.
- Ghostty VT parsing, true-color terminal state, styled text, cursor state,
  scrollback, and resize propagation.
- Hardware-accelerated Qt scene-graph text through public `QSGTextNode` nodes,
  with distance-field glyph atlases on hardware RHI backends. Batched colored
  geometry draws cell backgrounds, selections, cursors, and decorations.
- Keyboard, focus, mouse-reporting, bracketed-paste, and IME input paths.
- Mouse selection, double-click word selection, rectangular selection with
  `Alt`, clipboard copy, primary-selection paste, and an unsafe-paste review
  dialog.
- Tabs, recursively nested right/down splits, pane navigation, and confirmation
  before terminating running processes.
- OSC title and working-directory updates, used for tab titles and as the
  starting directory of a new split.

## Requirements

- Linux with a Wayland session and the Qt Wayland platform plugin.
- Qt 6.8 or newer with Core, Gui, Qml, Quick, Quick Controls 2, and Qt Test
  development components.
- A C++20 compiler, CMake 3.24 or newer, and Ninja.
- Zig **exactly 0.15.2**. `zig version` must print `0.15.2`.
- Git and `tic` (normally supplied by ncurses development/tools packages).
- Linux PTY headers and `libutil`.

The first Ghostty build may download its Zig dependencies and can take several
minutes.

## Project-local Zig

The checked-in presets use `.local/bin/zig`. The entire `.local` directory is
ignored by Git, so the compiler and its standard library stay with this
checkout without becoming repository content. The expected layout is:

```text
.local/bin/zig -> ../toolchains/zig-0.15.2/zig
.local/toolchains/zig-0.15.2/
```

To create it from the official Linux archive in a fresh checkout:

```sh
mkdir -p .local/bin .local/toolchains
tar -xf /path/to/zig-x86_64-linux-0.15.2.tar.xz -C .local/toolchains
mv .local/toolchains/zig-x86_64-linux-0.15.2 .local/toolchains/zig-0.15.2
ln -s ../toolchains/zig-0.15.2/zig .local/bin/zig
./.local/bin/zig version
```

## Get the pinned Ghostty source

The expected source is the `ghostty` submodule at commit
`c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3`:

```sh
git submodule update --init --recursive
git -C ghostty rev-parse HEAD
```

Configuration rejects a different revision when Git metadata is available.
`GHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY=ON` exists for intentional upgrade work, not
for normal builds. An external checkout can instead be selected with
`-DGHOSTTY_SOURCE_DIR=/path/to/ghostty`.

## Build

After setting up the project-local Zig toolchain, use one of the checked-in
presets:

```sh
cmake --preset dev
cmake --build --preset dev --parallel
```

For an optimized C++ build:

```sh
cmake --preset release
cmake --build --preset release --parallel
```

The developer and release presets both drive Ghostty's source-tree `zig-out`
directory. Do not build the two presets concurrently. If the pinned Ghostty
revision or Zig configuration changes, remove the affected CMake build directory
and Ghostty's generated `zig-out` before rebuilding.

Tests are enabled by both presets. To configure manually without them, pass
`-DGHOSTTY_QT_BUILD_TESTS=OFF`.

## Run

From a Wayland session:

```sh
./build/dev/ghostty-qt
```

With no command, a pane starts `$SHELL` when it names an executable, falling
back to `/bin/sh`. To run a specific command, put it after `--`:

```sh
./build/dev/ghostty-qt --working-directory "$PWD" -- /bin/bash -l
./build/dev/ghostty-qt --hold -- /bin/sh -c 'printf "done\n"'
```

`--hold` leaves the exited pane visible with its exit status. A command supplied
on the command line applies to the initial pane only; later tabs and splits start
the default shell.

### Command-line options

| Option | Meaning |
| --- | --- |
| `-h`, `--help` | Show command-line help. |
| `-v`, `--version` | Show the application version. |
| `--working-directory DIR` | Start the initial command in `DIR`; the directory must exist. |
| `--font-family FAMILY` | Select the terminal font family. |
| `--font-size POINTS` | Set the initial point size; the default is 12. |
| `--scrollback-lines LINES` | Set scrollback from 0 to 10,000,000 lines; the default is 10,000. |
| `--hold` | Keep the initial pane after its child exits. |
| `-- program arguments...` | Run a command instead of the default shell. |

### Shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+Shift+C` / `Ctrl+Shift+V` | Copy the selection / paste the clipboard. |
| `Ctrl+Shift+T` | Open a tab. |
| `Ctrl+Shift+E` / `Ctrl+Shift+O` | Split right / split down. |
| `Ctrl+Shift+W` | Close the active pane; closing the last pane closes its tab. |
| `Ctrl+Shift+Q` | Quit. |
| `Ctrl+PageUp` / `Ctrl+PageDown` | Select the previous / next tab. |
| `Alt+Arrow` | Focus the nearest pane in that direction. |
| `Ctrl++` / `Ctrl+-` / `Ctrl+0` | Increase, decrease, or reset the active pane's font size. |
| `Shift+PageUp` / `Shift+PageDown` | Scroll by one terminal page. |

Drag with the left mouse button to select; double-click to select a word and
hold `Alt` while dragging for a rectangular selection. `Shift` bypasses an
application's mouse-reporting mode so local selection and scrolling remain
available. Middle-click pastes the Wayland primary selection.

## Terminfo

The build generates Ghostty's `xterm-ghostty` entry and compiles it with `tic`
under `build/<preset>/share/terminfo`. Every child receives:

```text
TERM=xterm-ghostty
TERMINFO=<that preset's generated terminfo directory>
COLORTERM=truecolor
```

Inspect the generated entry with:

```sh
infocmp -A build/dev/share/terminfo -x xterm-ghostty
```

## Tests

```sh
ctest --preset dev
```

The suite covers option parsing, core `libghostty-vt` parse/render/input APIs,
a PTY-backed worker session, replacement of rendered terminal frames, and the
complete application's short-lived process/window lifecycle. Interactive tab,
split, selection, and dialog input are not yet fully automated.

For a headless QML startup smoke test, use the explicitly unsupported-backend
escape hatch:

```sh
GHOSTTY_QT_ALLOW_NON_WAYLAND=1 \
QT_QPA_PLATFORM=offscreen \
QT_QUICK_BACKEND=software \
timeout 3s ./build/dev/ghostty-qt --hold -- /bin/sh -c 'printf "smoke\n"'
```

The timeout status is expected because `--hold` keeps the window alive. This
path is for CI/smoke diagnostics only; normal use remains Wayland-only.

## Current limitations

- Renderer-v1 repopulates the scene-graph root from a full frame snapshot and
  lays out text per cell. This preserves exact terminal-grid placement, but
  retains CPU and allocation overhead that dirty-row reuse and run batching
  could reduce.
- Splits are fixed at 50/50; there are no draggable dividers.
- No X11 backend, multi-window support, configuration files, theme editor,
  search UI, session persistence, or relocatable production package yet.
- No Kitty graphics/inline images, color-emoji pipeline, or terminal-cell
  ligature shaping. Per-cell text rendering prioritizes correctness of the MVP
  architecture over advanced typography.
- Terminal-initiated clipboard writes are denied. User-initiated copy and paste
  are supported.
- The palette and core appearance are currently built in. Ghostty application
  configuration is not loaded; this project embeds `libghostty-vt`, not the
  complete Ghostty frontend.
- Automated startup testing uses Qt's software scene-graph backend. The GPU/RHI
  renderer still needs interactive visual qualification on real Wayland
  compositors and driver combinations.
