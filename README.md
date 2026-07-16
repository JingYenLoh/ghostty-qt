# ghostty-qt

`ghostty-qt` is a Linux, Wayland-only terminal emulator MVP built with Qt Quick,
C++20, and Ghostty's `libghostty-vt` C API. Ghostty supplies terminal parsing,
screen state, selection, and input encoding; a separate helper uses the pinned
Ghostty application parser for configuration; Qt supplies the window, controls,
scene-graph rendering, clipboard, and input-method integration.

This is an early developer build, not a drop-in replacement for the Ghostty
application. See [Architecture](docs/architecture.md) for the design and the
current tradeoffs, [Development and CI](docs/development.md) for the supported
build workflows, and [Feasibility and stack decision](docs/feasibility.md) for
the host-language comparison and remaining engineering risks.

## What works

- A real Linux PTY and a shell or command per pane.
- Ghostty VT parsing, true-color terminal state, styled text, cursor state,
  scrollback, and resize propagation.
- Hardware-accelerated Qt scene-graph text through public `QSGTextNode` nodes,
  with distance-field glyph atlases on hardware RHI backends. Color-batched
  scene-graph geometry draws cell backgrounds, selections, cursor shapes, and single,
  double, curly, dotted, and dashed underlines plus strike-through and overline
  decorations. Bold, faint, inverse, and invisible cell styles are retained;
  text blink is retained but deliberately not animated, matching the pinned
  Ghostty generic renderer.
- Keyboard, focus, mouse-reporting, bracketed-paste, and IME input paths.
- Mouse selection, double-click word selection, rectangular selection with
  `Alt`, clipboard copy, primary-selection paste, and an unsafe-paste review
  dialog.
- Tabs with indexed/last selection and cyclic reordering; recursively nested
  right/down splits with wrapped spatial/tree-order navigation, keybinding
  resize/equalize, split zoom, and close confirmation that distinguishes an
  idle interactive shell from a foreground job on its PTY.
- Stable tab/pane identities, a QML tab list model, and typed workspace actions;
  a catalog translates the currently implemented subset of Ghostty action
  strings into that action layer.
- OSC title and working-directory updates, used for tab titles and as the
  starting directory of a new split.
- Standard Ghostty configuration-file discovery, exact parsing and validation
  by the pinned Ghostty code, watched-file reload, and a deliberately small set
  of applied appearance/session keys.
- Ghostty's finalized keybindings—including named key tables, sequences,
  catch-all fallback, action chains, and local/`all`/`global` flags—for the
  supported pane/workspace actions. Linux global shortcuts use the XDG portal;
  focused input is matched before terminal encoding rather than through Qt's
  application-shortcut layer.

## Requirements

- Linux with a Wayland session and the Qt Wayland platform plugin.
- Qt 6.8 or newer with Core, D-Bus, Gui, Qml, Quick, Quick Controls 2, and Qt
  Test development components.
- A C++20 compiler, CMake 3.24 or newer, and Ninja.
- `pkg-config` and the libxkbcommon development package.
- Python 3.10 or newer for the parity-ledger test.
- Zig **exactly 0.15.2**. `zig version` must print `0.15.2`.
- Git and `tic` (normally supplied by ncurses development/tools packages).
- Linux PTY headers and `libutil`.

The first Ghostty build may download its Zig dependencies and can take several
minutes. The default build also produces a private Ghostty configuration-parser
library and helper, so its first build is substantially larger than rebuilding
the Qt application alone.

## Project-local Zig

The checked-in presets use `.local/bin/zig`. The entire `.local` directory is
ignored by Git, so the compiler and its standard library stay with this
checkout without becoming repository content. The expected layout is:

```text
.local/bin/zig -> ../toolchains/zig-0.15.2/zig
.local/toolchains/zig-0.15.2/
```

Install the exact toolchain from Zig's official release archive and verify its
published SHA-256 checksum with:

```sh
./scripts/bootstrap-zig.sh
./.local/bin/zig version
```

See [Development and CI](docs/development.md) for cache and mirror controls,
the sanitizer preset, and the CI build matrix.

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

## Parity tracking

[The Ghostty parity manifest](docs/ghostty-parity.json) records the supported
Linux/Wayland/Qt scope and inventories the pinned upstream configuration keys,
keybinding actions, and CLI actions. Most entries are intentionally still
marked as planned; the manifest is a coverage ledger, not a claim of current
feature parity.

Run its source-drift check directly with:

```sh
python3 scripts/check-ghostty-parity.py
```

Pass `--source /path/to/ghostty` when auditing an external checkout selected
with `GHOSTTY_SOURCE_DIR`; CTest receives the configured source automatically.

The check verifies the manifest, CMake pin, and Ghostty checkout agree on the
same revision, then extracts the three inventories again from the pinned
Ghostty source. It is also part of CTest as `ghostty-parity-manifest`.

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

## Ghostty configuration

At startup the application loads the standard Linux Ghostty files, in upstream
order:

```text
$XDG_CONFIG_HOME/ghostty/config
$XDG_CONFIG_HOME/ghostty/config.ghostty
```

If `XDG_CONFIG_HOME` is unset or relative, `$HOME/.config` is used. A private
`ghostty-qt-config-helper` runs the pinned Ghostty `+validate-config` and
`+show-config` actions and a project-private structured keybinding export, so
syntax, file precedence, `config-file` includes, canonical values, and the
finalized binding trie come from the exact pinned Ghostty parser rather than a
Qt-side reimplementation. The main process receives only typed value snapshots.

The current compatibility slice applies these keys:

| Key | Current behavior |
| --- | --- |
| `font-family` | Uses the first configured family. Explicit `--font-family` wins; the remaining Ghostty fallback list is not yet used. |
| `font-size` | Sets new panes and reloads existing panes unless they were manually zoomed. Explicit `--font-size` wins. |
| `foreground`, `background` | Set terminal defaults for new panes and apply live to existing terminals. |
| `palette` | Applies Ghostty's complete 256-color default palette. Terminal OSC 4 overrides survive a config reload; OSC 104 resets an entry to the newest configured default. |
| `selection-foreground`, `selection-background` | Apply fixed colors or Ghostty's cell foreground/background aliases. Unset values retain Ghostty's default terminal-foreground/terminal-background selection pairing. |
| `cursor-color`, `cursor-style`, `cursor-opacity`, `cursor-text` | Apply live cursor appearance, including fixed and cell-relative colors. Terminal OSC 12 and DECSCUSR overrides survive reload; their reset sequences reveal the newest configured defaults. |
| `cursor-style-blink` | Applies the configured default, but is not yet exact: the public `libghostty-vt` setter accepts only a boolean, so it cannot retain Ghostty's explicit true/false-versus-DEC-mode-12 distinction. |
| `bold-color`, `faint-opacity` | Apply Ghostty's bold foreground transformation and faint glyph/decorations opacity. |
| `theme` | A static theme's appearance values can flow through the canonical fields above when the pinned parser resolves them. Dynamic light/dark theme switching is not implemented. |
| `scrollback-limit` | Preserves Ghostty's byte-valued limit for new panes. Explicit `--scrollback-lines` wins. An existing libghostty terminal cannot resize its history allocation during reload. |
| `confirm-close-surface` | Supports `false`, `true`, and `always`, including live policy updates. `true` detects separate foreground jobs and latches submitted commands; shell builtins still need semantic prompt integration for exact detection. `always` confirms any live child. |
| `config-file` | Included files are parsed by Ghostty; existing files and directories for missing optional includes are watched for reload. |
| `keybind` | Loads Ghostty's finalized structured root and named-table sets. Per-pane table stacks and one-shot tables, shared-prefix sequences, physical/Unicode/catch-all lookup, action chains, `unconsumed`/`performable`, byte-exact invalid-sequence replay, `end_key_sequence`, and process-wide `all`/`global` dispatch are supported for the implemented action subset. Root, direct, single-action `global` bindings are also registered through the Linux XDG Global Shortcuts portal. |

The service watches the two standard paths, their containing directories, and
included-file paths. Changes are debounced and parsed away from the GUI thread.
It validates before and after extracting values, retries failures so a newly
created required include can recover without touching the root file, and logs
successful helper warnings. A validation or helper failure leaves the last
valid snapshot active when one exists; otherwise the built-in/CLI options
remain in use. Most Ghostty keys are still tracked as planned in the parity
manifest and are not silently treated as implemented.

Named tables use Ghostty's normal configuration syntax and remain local to
each pane. For example:

```ini
keybind = ctrl+shift+m=activate_key_table:modal
keybind = modal/r=reload_config
keybind = modal/escape=deactivate_key_table
```

`all:` runs an action over every surface in the process; app-scoped actions
such as `reload_config` run once. `global:` has the same fanout semantics and,
for eligible root bindings, additionally registers with the desktop portal:

```ini
keybind = all:ctrl+shift+f=increase_font_size:1
keybind = global:super+shift+r=reload_config
```

Portal support depends on the compositor/desktop implementation. Registration
failure is nonfatal and has no fake `QShortcut` fallback. Matching the pinned
Linux frontend, portal registration accepts only direct root bindings with one
trigger and one action; focused in-app dispatch still handles action chains.

### Command-line options

| Option | Meaning |
| --- | --- |
| `-h`, `--help` | Show command-line help. |
| `-v`, `--version` | Show the application version. |
| `--working-directory DIR` | Start the initial command in `DIR`; the directory must exist. |
| `--font-family FAMILY` | Select the terminal font family. |
| `--font-size POINTS` | Set the initial point size; the default is 12. |
| `--scrollback-lines LINES` | Estimate capacity for 0 to 10,000,000 rows; the default is 10,000. The legacy value is converted to libghostty's byte cap. |
| `--hold` | Keep the initial pane after its child exits. |
| `-- program arguments...` | Run a command instead of the default shell. |

### Shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+Shift+C` / `Ctrl+Shift+V` | Copy the selection / paste the clipboard. |
| `Ctrl+Shift+T` | Open a tab. |
| `Ctrl+Shift+O` / `Ctrl+Shift+E` | Split right / split down. |
| `Ctrl+Shift+W` | Close the active tab. |
| `Ctrl+Shift+Q` | Quit. |
| `Ctrl+Shift+Tab` / `Ctrl+Tab` | Select the previous / next tab. |
| `Alt+1` … `Alt+8` / `Alt+9` | Select a numbered tab / the final tab. |
| `Ctrl+Shift+PageUp` / `Ctrl+Shift+PageDown` | Move the active tab backward / forward. |
| `Ctrl+Super+[` / `Ctrl+Super+]` | Focus the previous / next split in tree order. |
| `Ctrl+Alt+Arrow` | Focus the nearest pane in that direction. |
| `Super+Ctrl+Shift+Arrow` | Move the nearest matching split divider by 10 pixels. |
| `Ctrl+Shift+Enter` | Toggle zoom for the active split. |
| `Ctrl++` / `Ctrl+-` / `Ctrl+0` | Increase, decrease, or reset the active pane's font size. |
| `Shift+PageUp` / `Shift+PageDown` | Scroll by one terminal page. |

These are the pinned Ghostty Linux defaults when configuration integration is
enabled. Custom bindings can also invoke `goto_tab`, `last_tab`, `move_tab`,
`goto_split`, `resize_split`, `equalize_splits`, and `toggle_split_zoom`
directly.

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

An installed copy keeps its private database under
`${CMAKE_INSTALL_DATADIR}/ghostty-qt/terminfo` and resolves that directory
relative to the running executable, so the installation prefix can be moved.
For diagnostics or nonstandard layouts, set `GHOSTTY_QT_TERMINFO` to a directory
containing a compiled `xterm-ghostty` entry. An explicit invalid override is an
error rather than falling back silently to another database.

## Tests

```sh
ctest --preset dev
```

The suite covers option and config-overlay parsing, core `libghostty-vt`
parse/render/input APIs, the C++ VT adapter contract, a PTY-backed worker
session (including idle-shell/foreground-job transitions), stable workspace IDs
and typed action dispatch, Ghostty action-string translation, full and
dirty-row terminal updates, config watching and generation-safe last-good
asynchronous reload behavior, structured keybinding trie matching with Linux
physical-key locations, named-table stacks, process-wide fanout, portal
race/reload coverage, and PTY-backed sequence replay, helper-process
protocol/error handling, real-parser `clear`/`unbind` resolution, the
machine-checked parity manifest, the complete
application's short-lived process/window lifecycle, and staged relocation of
both terminfo and the private config helper. The real QML close dialog is also
exercised headlessly; workspace tab ordering, split layout, navigation, and
zoom are covered through typed actions, while interactive selection and
unsafe-paste dialog input are not yet fully automated.

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

- Renderer-v1 sends only dirty rows across the worker/UI boundary after its
  initial frame, but it still rebuilds scene-graph children and text layouts
  for each presented update. Persistent row nodes and run batching remain
  CPU/allocation optimizations.
- Split ratios can be resized and equalized through Ghostty keybindings, but
  there are no draggable dividers yet.
- No X11 backend, multi-window support, theme editor, search UI, session
  persistence, or production package metadata yet. Configuration support is
  limited to the documented compatibility slice; most Ghostty keys remain
  planned.
- Configured bindings support finalized root and named tables, including
  sequences, catch-all triggers, chains, local consume/performability,
  `all` fanout, and XDG-portal-backed `global` registration for the documented
  action subset. Remaining gaps are primarily unimplemented actions; the
  portal also intentionally shares the pinned Linux frontend's single-action
  root-binding restriction.
- Linux/Wayland native scan codes preserve physical-key identity for bindings
  and terminal input. Shifted-punctuation fallback matching is currently
  US-layout-oriented because public `QKeyEvent` data does not include the
  compositor keymap's unmodified layout level.
- No Kitty graphics/inline images, color-emoji pipeline, or terminal-cell
  ligature shaping. Per-cell text rendering prioritizes correctness of the MVP
  architecture over advanced typography.
- Terminal-initiated clipboard writes are denied. User-initiated copy and paste
  are supported.
- The first appearance slice covers the full palette, selection, cursor,
  bold, and faint settings documented above. Dynamic light/dark theme
  switching, font fallback lists, palette generation/harmonization, and the
  rest of Ghostty's appearance model remain planned. The pinned
  `+show-config` text boundary does not expose the derived palette and explicit
  entry mask needed to reproduce `palette-generate` or `palette-harmonious`
  exactly.
- Automated startup testing uses Qt's software scene-graph backend. The GPU/RHI
  renderer still needs interactive visual qualification on real Wayland
  compositors and driver combinations.
