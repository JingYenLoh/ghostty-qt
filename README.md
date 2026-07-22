# ghostty-qt

`ghostty-qt` is a Linux, Wayland-only terminal emulator MVP built with Qt Quick,
C++23, and Ghostty's `libghostty-vt` C API. Ghostty supplies terminal parsing,
screen state, selection, and input encoding; a separate helper uses the pinned
Ghostty application parser for configuration and selected CLI actions; Qt
supplies the window, controls, scene-graph rendering, clipboard,
and input-method integration.

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
  `Alt`, keyboard select-all/endpoint adjustment, configurable trailing-space
  trimming and copy-on-select destinations, primary-selection fallback,
  selection clearing after explicit copy or typed input, configurable
  middle-click paste, and worker-authoritative unsafe-paste review with
  correlated confirmation.
- Explicit OSC 8 hyperlinks and Ghostty's default regex-detected URLs/paths,
  with `Ctrl`-hover pointer/underline feedback, release-validated `Ctrl`-click
  opening through the desktop URL handler, and the `copy_url_to_clipboard`
  keybinding action. The `link-url` setting applies live to regex links while
  OSC 8 remains independent. Copy preserves the exact destination or matched
  UTF-8 bytes rather than the Qt URL adapted for opening. `link-previews`
  controls a pane-local destination overlay for the already accepted hover;
  its policy also reloads live without rescanning while the pointer remains on
  that terminal link.
- Pane-local terminal search across the active screen and its scrollback, with
  progressive match counts, overlapping literal matches, next/previous
  navigation, selected-result scrolling, and candidate/selected highlights.
  The top-right Qt search overlay supports the pinned Ghostty search actions
  and live-reloaded fixed or cell-relative search colors.
- Full-height, fractional, line, absolute-row, top/bottom, and
  selection-targeted scrollback navigation through Ghostty actions.
- Tabs with indexed/last selection and cyclic reordering; recursively nested
  directional and aspect-selected automatic splits with wrapped spatial and
  tree-order navigation, keybinding resize/equalize, exact-gap pointer-dragged
  dividers, live Ghostty-compatible unfocused-pane dimming, split zoom with
  optional navigation-preserved transfer, and close confirmation that
  distinguishes an idle interactive shell from a foreground job on its PTY.
  Per-pane read-only mode blocks surface-originated
  user input while output, terminal protocol replies, focus bookkeeping, and
  terminal-local selection, copy, search, and scrolling remain available; an
  input-transparent top-right badge exposes the state, which always protects
  that pane during close.
- Stable tab/pane identities, a QML tab list model, and typed workspace actions;
  a catalog translates the currently implemented subset of Ghostty action
  strings into that action layer. `set_surface_title` replaces the originating
  pane's base title, including with an explicit empty value, until the next
  terminal title update. `prompt_surface_title` adds a persistent per-pane
  override above that base, and `copy_title_to_clipboard` consumes exactly
  those two raw surface layers. The direct `set_tab_title` action applies a
  still higher persistent override to the source pane's tab by stable
  identity. Both prompt actions share one modal Qt dialog and stable FIFO
  scheduler. Surface-scoped fullscreen and maximize actions target the
  containing Qt window, with broad bindings coalesced once per window.
- Process-owned multiwindow lifetime and aggregate quit, including resident
  zero-window operation. Eligible bare secondary launches and desktop shells
  share the standard `org.freedesktop.Application` session-D-Bus endpoint; an
  activating launcher synchronously creates exactly one new primary-owned
  window without forwarding command text, while `initial-window=false` exits
  without activation. Standard activation-token and desktop-startup-ID data
  follow the originating request into that window without leaking to its
  shell. Installed desktop/service metadata supports cold and warm activation,
  and Debug and Release builds use separate identities.
- OSC title and local-host-validated working-directory updates, used for tab
  titles and—when the corresponding tab/split inheritance policy permits
  it—as the starting directory of a new surface.
- Standard Ghostty configuration-file discovery, exact parsing and validation
  by the pinned Ghostty code, watched-file reload, and a deliberately small set
  of applied appearance/session keys.
- Transparent pre-Qt delegation of the pinned `+edit-config`,
  `+explain-config`, `+help`, `+list-actions`, `+list-colors`,
  `+list-keybinds`, `+show-config`, and `+validate-config` implementations.
  These retain the caller's terminal, streams, process relationship,
  environment, working directory, and exact action exit status without
  requiring a working Wayland or Qt platform plugin.
- Ghostty's finalized keybindings—including named key tables, sequences,
  catch-all fallback, action chains, and local/`all`/`global` flags—for the
  supported pane/workspace actions. Linux global shortcuts use the XDG portal;
  focused input is matched before terminal encoding rather than through Qt's
  application-shortcut layer.

## Requirements

- Linux with a Wayland session and the Qt Wayland platform plugin.
- Qt 6.8 or newer with Core, D-Bus, Gui, Qml, Quick, Quick Controls 2, and Qt
  Test development components.
- A C++23 compiler and standard library, CMake 3.24 or newer, and Ninja.
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

The authoritative commit is recorded in
[`GHOSTTY_REVISION`](GHOSTTY_REVISION), and the `ghostty` submodule must point
to that official upstream revision:

```sh
cat GHOSTTY_REVISION
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

[Features requiring new official Ghostty APIs](REQUIRES_UPSTREAM.md) are
documented separately. The submodule remains unmodified until those APIs are
available from an official upstream commit.

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
cmake --build --preset dev -j"$(nproc)"
```

For an optimized C++ build:

```sh
cmake --preset release
cmake --build --preset release -j"$(nproc)"
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
on the command line applies to the initial pane only; later tabs, splits, and
windows start the default shell.

## Ghostty configuration

At startup the application loads the standard Linux Ghostty files, in upstream
order:

```text
$XDG_CONFIG_HOME/ghostty/config
$XDG_CONFIG_HOME/ghostty/config.ghostty
```

If `XDG_CONFIG_HOME` is unset or relative, `$HOME/.config` is used. A private
`ghostty-qt-config-helper` runs the pinned Ghostty `+validate-config` and
`+show-config` actions and a project-private structured config export, so
syntax, file precedence, `config-file` includes, canonical values, and the
finalized binding trie, lossless nullable lifetime values, and raw
`gtk-single-instance` mode plus the `initial-window` startup decision come
from the exact pinned Ghostty parser rather than a Qt-side reimplementation.
The main process receives only typed value snapshots.

The current compatibility slice applies these keys:

| Key | Current behavior |
| --- | --- |
| `working-directory` | Sets the default directory for initial panes and the fallback for new tabs, splits, and windows. Ghostty's helper finalizes `home` and `~/...`; empty/`inherit` preserves the launching process cwd and logical `PWD`. Concrete path spelling is retained, including symlink-sensitive `..`. Explicit `--working-directory` wins. As in pinned Ghostty, an unavailable concrete directory starts the child in the process directory while retaining the requested logical `PWD` until OSC 7 corrects it. Because the helper itself is invoked with CLI arguments, an unset desktop launch currently resolves as `inherit` rather than GTK Ghostty's context-dependent `home` default. |
| `split-inherit-working-directory` | Defaults to `true`. A future split then uses its explicit source pane's latest accepted local OSC 7 directory, falling back to `working-directory` when the terminal has none. `false` always uses `working-directory`. Reload affects future splits only and remains independent of font-size inheritance. The split policy is implemented, but exact unset desktop fallback remains partial with `working-directory`. |
| `split-preserve-zoom` | Ghostty's canonical `no-navigation` default clears split zoom after a successful `goto_split`. Canonical `navigation` instead transfers the zoomed presentation to the newly focused pane for successful previous/next or spatial navigation. Reload affects subsequent navigation immediately. A direction with no target changes neither focus nor zoom; direct activation of another pane and structural changes such as creating a split retain their existing unzoom behavior. |
| `tab-inherit-working-directory` | Defaults to `true`. A new tab uses the action-target pane, or the current tab's active pane for the QML button, and inherits its latest accepted local OSC 7 directory. `false` or a cleared/unavailable report uses the newest `working-directory`. Reload changes future tab creation only; existing sessions are never moved. The shared unset desktop fallback and non-UTF-8 path transport limitations keep the policy partial. |
| `window-inherit-working-directory` | Defaults to `true`. A pane-originated new window inherits that exact pane's latest accepted local OSC 7 directory; an application/global action uses the focused or most recently active live pane, and a stale or zero-window source uses the newest `working-directory`. `false` always uses that application fallback. Reload affects future windows only. The shared unset desktop fallback and non-UTF-8 path transport limitations keep the policy partial. |
| `window-inherit-font-size` | Defaults to `true`. New tabs and pane-originated windows inherit only the source pane's actual point size, including manual zoom; their font family still comes from current configuration. An application/global new-window action uses the focused or most recently active pane. `false` uses the newest effective `font-size`, including explicit CLI precedence. The child starts unadjusted and therefore follows later font-size reloads. |
| `window-new-tab-position` | Supports Ghostty's exact `current` and `end` values and defaults to `current`. `current` inserts after the tab selected immediately before creation, or appends when no tab is selected; `end` always appends. The new tab becomes selected. Placement is independent of the action-target pane retained for directory and font inheritance, and reload affects future tabs only. |
| `window-show-tab-bar` | Supports Ghostty's exact `always`, `auto`, and `never` values and defaults to `auto`. `always` shows the tab strip with any tab count, `auto` hides it for one tab and shows it at two or more, and `never` hides it. Reload and the one/two-tab boundary update visibility live. This Qt mapping hides only the QML `TabBar`; the surrounding toolbar and its new-tab, split, and close controls remain available. |
| `window-width`, `window-height` | Define the initial terminal grid in cells when both values are nonzero; one-sided values retain the ordinary Qt default. Ghostty's finalized 10-column by 4-row minimum is also used as the window's cell-derived resize minimum, bounded when even that minimum cannot fit the available screen. Every new window uses its resolved font size—including pane-inherited manual zoom—adds the persistent Qt toolbar, inherits the originating/focused window's screen when available, and clamps best-effort to that screen. Before libghostty or `forkpty` is created, the first pane snapshots the resulting logical viewport and device scale so an immediately executing child observes the same rows, columns, and PTY pixel size; later tabs and splits do not inherit this window seed. Reload affects future windows only. A maximized/fullscreen compositor can still replace the hidden normal size only after mapping, which arrives as the ordinary first live resize. |
| `maximize` | Defaults to `false`. `true` presents every subsequently created window maximized. Reload changes the default for future local, resident-replacement, and desktop-activated windows without changing any existing window state. |
| `fullscreen` | Defaults to `false`. On Linux, `true` and every pinned non-native variant present subsequently created windows in Qt's native fullscreen mode. When `maximize` is also true, leaving fullscreen through either the application action or compositor restores the window to maximized. Reload affects only future windows. |
| `initial-window` | Defaults to `true`. `false` completes startup without constructing a QML root or terminal surface, while process configuration, reload, application actions, global shortcuts, and single-instance ownership remain available. A false secondary still participates in atomic name arbitration but exits successfully without activating the owner; a later launcher whose own current value is true activates that same primary even if the primary started with false. The installed D-Bus service forces false only for its bootstrap process, then the queued standard activation unconditionally creates exactly one first window. Local/application `new_window` and every accepted process/desktop activation remain unconditional, and the first surface created after suppressed startup retains the current one-shot program/hold; later surfaces clear them. Reloading the primary's value does not itself create or close a window because the decision is sampled once per launching process. Explicit `--initial-window=true|false` startup values take precedence over configuration. |
| `gtk-single-instance` | Preserves raw `false`, `true`, or `detect`; detect uses the originating process's real invocation and `TERM_PROGRAM`. Eligible no-payload launches atomically arbitrate the application-ID D-Bus name and expose `org.freedesktop.Application` at the ID-derived root path. A true launcher or standard desktop `Activate(a{sv})` call keeps a startup-time reply pending and succeeds only after the primary synchronously registers one complete window/workspace pair; a false launcher exits successfully without sending activation. The installed direct-exec service cold-starts a forced true/false zero-window host, while the desktop fallback forces only true. CLI bootstrap values outrank contradictory user configuration. The standard string `activation-token` and `desktop-startup-id` platform fields retain FIFO request identity, are projected only while the corresponding window is shown, and are then cleared; wrong types, embedded NULs, and unknown fields are discarded, and shell children never inherit the values. A direct primary captures the matching launcher environment before Qt starts and consumes it through the same presentation path. The created window uses the primary's latest config, optionally inherits an actually focused/last-focused cwd, retains configured font size, and uses configured cwd when no valid focus remains. If it is the primary's first-ever surface it retains one-shot program/hold; subsequent surfaces clear them. An unavailable bus starts locally; connected-bus failures fail closed, while bounded owner-handoff recovery follows a replacement primary. `Open`, `ActivateAction`, custom `class` namespaces, general option-bearing launches, and payload forwarding remain incomplete. The synchronous creation acknowledgement deliberately strengthens pinned Ghostty's fire-and-forget activation. |
| `quit-after-last-window-closed` | Defaults to `true` on Linux. Qt's implicit last-window exit is disabled so only the final ordinary confirmed window closure enters this application-owned policy: `true` exits on the next event turn when no delay is set, while `false` retires every root and worker but keeps process configuration, actions, global shortcuts, and any enabled activation endpoint resident. Matching the pinned GTK control flow, an `initial-window=false` process that has never requested a surface is not treated as having closed its last window and remains resident even when this setting is true. After a first surface exists, the ordinary policy applies. An in-process, portal, or eligible true bare-secondary `new_window` request can create a fresh QML window, and reload updates both all live windows and the next replacement. A command supplied after `--` forces `true`, matching Ghostty's `-e` lifecycle. |
| `quit-after-last-window-closed-delay` | Applies Ghostty's Linux delay after the final ordinary window close. The structured helper preserves null versus configured zero and performs Ghostty's exact whole-millisecond truncation and `c_uint` saturation. A successfully created replacement window cancels the single application timer; a failed factory attempt leaves it armed, identical reloads preserve its deadline, changed reloads reconcile it, and explicit `quit`—including with zero windows—bypasses it. Pinned Ghostty's config documentation says a never-windowed `initial-window=false` process should time out, but its GTK runtime exposes no startup timer hook; this frontend follows the executable pinned GTK behavior and starts the delay only after a real final surface closes. |
| `font-family` | Uses the first configured family. Explicit `--font-family` wins; the remaining Ghostty fallback list is not yet used. |
| `font-size` | Sets new panes and reloads existing panes unless they were manually zoomed. New tabs may initially inherit their source's actual size as described above. Explicit `--font-size` wins. |
| `foreground`, `background` | Set terminal defaults for new panes and apply live to existing terminals. |
| `unfocused-split-opacity` | Sets the retained content opacity of unfocused panes in a split, clamped to Ghostty's `0.15`–`1.0` range. The Qt renderer composites the complementary fill alpha and updates existing panes live. |
| `unfocused-split-fill` | Sets the optional fixed RGB dimming fill. Unset resolves live to the configured `background`, independently of terminal OSC 11 overrides. Search suppresses dimming for its pane. |
| `split-divider-color` | Applies Ghostty's optional fixed RGB color live to existing and future split dividers. Unset preserves the Qt frontend's ordinary two-pixel gap color. |
| `palette` | Applies Ghostty's complete 256-color default palette. Terminal OSC 4 overrides survive a config reload; OSC 104 resets an entry to the newest configured default. |
| `selection-foreground`, `selection-background` | Apply fixed colors or Ghostty's cell foreground/background aliases. Unset values retain Ghostty's default terminal-foreground/terminal-background selection pairing. |
| `search-foreground`, `search-background` | Apply fixed or cell-relative colors to search candidates. The defaults are black on `#FFE082`, matching Ghostty. |
| `search-selected-foreground`, `search-selected-background` | Apply fixed or cell-relative colors to the selected search result. The defaults are black on `#F2A57E`, matching Ghostty. Normal terminal selection still has render priority. |
| `cursor-color`, `cursor-style`, `cursor-opacity`, `cursor-text` | Apply live cursor appearance, including fixed and cell-relative colors. Terminal OSC 12 and DECSCUSR overrides survive reload; their reset sequences reveal the newest configured defaults. |
| `cursor-style-blink` | Applies the configured default, but is not yet exact: the public `libghostty-vt` setter accepts only a boolean, so it cannot retain Ghostty's explicit true/false-versus-DEC-mode-12 distinction. |
| `bold-color`, `faint-opacity` | Apply Ghostty's bold foreground transformation and faint glyph/decorations opacity. |
| `theme` | A static theme's appearance values can flow through the canonical fields above when the pinned parser resolves them. Dynamic light/dark theme switching is not implemented. |
| `scrollback-limit` | Preserves Ghostty's byte-valued limit for new panes. Explicit `--scrollback-lines` wins. An existing libghostty terminal cannot resize its history allocation during reload. |
| `confirm-close-surface` | Supports `false`, `true`, and `always`, including live policy updates. `true` detects separate foreground jobs and latches submitted commands; shell builtins still need semantic prompt integration for exact detection. `always` confirms any live child. |
| `clipboard-trim-trailing-spaces`, `copy-on-select`, `selection-clear-on-copy` | Apply live to selection copying. Linux `copy-on-select` accepts Ghostty's `false`, `true` (primary selection), and `clipboard` (primary and standard clipboard) modes, with standard-clipboard fallback when primary selection is unavailable. Explicit copy can clear only after formatting; automatic copy never clears. |
| `clipboard-paste-protection`, `clipboard-paste-bracketed-safe` | Default to `true` and apply live. The session worker uses Ghostty's current bracketed-paste mode, exact safety check, and encoder. Unsafe text is retained under a correlated request ID; cancellation is inert, while confirmation rechecks current terminal mode before encoding and returns to the active screen before writing. |
| `selection-clear-on-typing` | Defaults to `true` and applies live. A non-modifier key clears only after it produces terminal bytes; encoded repeats/releases follow the same rule, and physical Escape clears even when configured `false`. IME commit and preedit transitions participate, while consumed bindings, sequence-leader replay, raw `text`/`csi`/`esc` actions, and paste do not. |
| `middle-click-action` | Applies `primary-paste` or `ignore` live. Primary paste reads the standard clipboard in `copy-on-select=clipboard` mode; otherwise it reads the primary selection with standard fallback. Terminal mouse reporting takes precedence. |
| `mouse-reporting` | Defaults to `true` and gates application-requested DEC mouse capture independently for each pane. `false` leaves the terminal's requested mode intact while restoring local selection and scrolling. Every pointer event rechecks the policy/DEC conjunction; reported buttons and wheels atomically clear terminal selection. Reload replaces any pane-local runtime toggle, while new tabs and splits use the configured value. |
| `link-url` | Enables Ghostty's pinned default regex matcher for scheme URLs and file paths. The default is `true`; live reload recomputes hover state. Explicit OSC 8 hyperlinks are unaffected. |
| `link-previews` | Controls the destination overlay for an accepted `Ctrl` hover. `true` (the default) previews OSC 8 and regex links, `false` previews neither, and `osc8` previews only explicit OSC 8 destinations. Reload is frontend-only and preserves a hover over the terminal link. Removing a preview while its bottom-left guard owns the pointer resumes physical terminal hit testing and may query that newly exposed cell. |
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
such as `new_window`, `open_config`, `reload_config`, and `quit` run once.
`global:` has the same fanout semantics and, for eligible root bindings,
additionally registers with the desktop portal:

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

The default configuration-enabled build also accepts these exact Ghostty CLI
actions:

```text
+edit-config     +explain-config  +help          +list-actions
+list-colors     +list-keybinds    +show-config   +validate-config
```

They may use their pinned Ghostty action-specific flags and operands in the
original order. The frontend recognizes them from raw process arguments and
replaces itself with its installed sibling helper before Qt starts, so piping,
pagers, TTY detection, process/signal relationships, and exit codes behave like
a direct CLI invocation. `-e` and this frontend's documented `--` delimiter
protect command payloads from action detection: `--` launches the documented
frontend command, while an earlier exact `-e` suppresses delegation according
to pinned Ghostty's detector ordering but remains unsupported by the frontend
launch parser.
Ordinary `--help` and `--version` remain the ghostty-qt frontend variants;
unsupported `+` actions fail explicitly instead of being interpreted as
terminal programs.

`+help` and `+list-actions` print pinned upstream catalogs, including entries
that remain planned in ghostty-qt; appearing in those catalogs is not a claim
that the frontend implements the item. `+show-config` and `+validate-config`
run the pinned implementations through ghostty-qt's embedded-runtime helper,
whose application runtime is intentionally `none` rather than GTK. A
development build configured with `GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG=OFF` omits
that helper and reports delegated actions as unavailable before attempting GUI
startup.

`+edit-config` is the pinned CLI action, distinct from the GUI `open_config`
keybinding action. It loads the standard configuration (creating the preferred
file when neither standard file exists), selects the first non-empty `$VISUAL`
then `$EDITOR` value, and replaces the process with `/bin/sh -c` so the editor
value may contain shell syntax. It opens the preferred non-empty
`config.ghostty`, otherwise the non-empty legacy `config`, otherwise the new
path. At the pinned revision, that newly created file is empty despite the
upstream template-writing intent. The command does not reload configuration
after editing.

### Shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+Shift+C` / `Ctrl+Shift+V` | Copy the selection / paste the clipboard. |
| `Ctrl+Shift+A` | Select all terminal content. |
| `Ctrl+Shift+F` | Open and focus the current pane's search overlay. |
| `Ctrl+,` | Create if needed and open the Ghostty configuration file. |
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
| `Ctrl+Enter` | Toggle fullscreen for the containing window. |
| `Ctrl+Shift+Enter` | Toggle zoom for the active split. |
| `Ctrl++` / `Ctrl+-` / `Ctrl+0` | Increase, decrease, or reset the active pane's font size. |
| `Shift+PageUp` / `Shift+PageDown` | Scroll by one terminal page. |
| `Shift+Arrow` | Adjust an existing selection endpoint. |

These are the pinned Ghostty Linux defaults when configuration integration is
enabled. Custom bindings can also invoke `goto_tab`, `last_tab`, `move_tab`,
`new_split:left|right|up|down|auto` (with an omitted direction also selecting
`auto`), `close_surface`, `close_tab[:this|other|right]`, `goto_split`,
`resize_split`, `equalize_splits`, and
`toggle_split_zoom` directly. `toggle_maximize` toggles the containing window
between maximized and normal state; Ghostty has no default binding for it.
`toggle_readonly` and
`toggle_mouse_reporting` apply independently to their source pane; `all:` or
`global:` applies either action once to every stable pane target. Mouse
reporting remains the conjunction of the pane policy and the terminal's DEC
mouse mode, with the effective route reevaluated for every pointer event. Raw
DEC capture remains authoritative for Ghostty's Shift hyperlink escape even
when the reporting policy is disabled.
Tab-close actions resolve from the originating pane's stable containing tab.
`other` closes every other tab and `right` closes only tabs after the source;
valid no-target forms are harmless performed actions. A Qt batch confirmation
freezes the target IDs, so later moves and insertions cannot redirect it.
Each close dialog has a nonzero request identity, preventing a delayed response
from confirming or cancelling a newer request.
`close_surface` is a strict void action anchored to the originating pane's
stable identity. Closing an active split focuses the previous pane in tree
order, except that closing the leftmost pane focuses the next one; closing an
inactive split preserves the current focus. The split tree collapses around
the removed leaf, and a final leaf naturally promotes the operation through
tab removal and, for the final tab, the application shutdown path. Broad
`all:` and `global:` forms intentionally converge on one atomic close request
per workspace rather than presenting one dialog per surface.
`set_surface_title:<title>` decodes Ghostty's escaped UTF-8 transport and
replaces the source pane's base title without changing focus or terminal
selection. A colon is required, and `set_surface_title:` is a present empty
title rather than the launch-program fallback. Later OSC title updates replace
the action value; an existing tab-title override continues masking either base
value until it is cleared. Broad bindings apply the action once to every pane.
`prompt_surface_title` is a strict void action that snapshots the source pane's
raw surface override, base title, or empty value—never its tab or launch
fallback. Non-empty OK text becomes a persistent per-pane override without
trimming; empty OK clears it and Cancel is inert. OSC and `set_surface_title`
continue updating the hidden base until that override is cleared.
`copy_title_to_clipboard` is also a strict void action. It copies that same raw
surface override-or-base value exactly to the standard clipboard, preserving
Unicode and surrounding whitespace. An absent or empty title is a no-op; tab
overrides, zoom and fallback labels, selection-copy trimming/clearing, and the
primary selection do not participate. Broad fanout visits every pane in the
existing stable tab/tree order, so the last non-empty surface title wins. That
order is deterministic but differs from Ghostty's process-wide
creation/swap-remove surface vector after tab reordering or pane deletion.
`set_tab_title:<title>` installs a stable override on the source pane's tab;
`set_tab_title:` clears it and reveals the active pane's effective surface
title. `prompt_tab_title` is a strict void action—spellings with a colon are
invalid—and targets the source pane's tab even if tabs are selected or moved
while its dialog is open. The field snapshots the raw override when present;
otherwise it snapshots the current display title, including the modeled zoom
prefix. It receives focus with the caret at the end without selecting the
existing text. OK preserves the entered text exactly, an empty value clears
the override, and Cancel leaves it unchanged. Requests run one at a time in
FIFO order shared with surface-title prompts; `all:` and `global:` enqueue one
request per surface without deduplicating split panes in the same tab. A
surface prompt is cancelled if its exact pane closes. Closing only the
originating pane does not redirect a tab prompt when its tab survives, while
deleting the target tab cancels its queued prompts and makes stale dialog
completions inert.
Successful `goto_split` actions consult the live `split-preserve-zoom` policy:
`navigation` transfers an existing zoom to the destination, while the default
`no-navigation` clears it. An unsuccessful navigation is inert.
Viewport/selection bindings additionally support
`scroll_to_top`, `scroll_to_bottom`, `scroll_to_row`, `scroll_page_up`,
`scroll_page_down`, `scroll_page_fractional`, `scroll_page_lines`,
`scroll_to_selection`, `select_all`, and `adjust_selection`. Terminal-control
bindings support `csi`,
`esc`, `text`, and `reset`; `copy_url_to_clipboard` copies the explicit OSC 8
destination or default-regex match currently accepted under the pointer.
Raw-write actions return the viewport to the active area, while reset clears
emulator state without sending bytes to the child.

Font bindings support `increase_font_size:<points>`,
`decrease_font_size:<points>`, `set_font_size:<points>`, and
`reset_font_size`. Numeric payloads use Ghostty's required f32 action grammar.

Search bindings support `start_search`, `end_search`, `search:<text>`,
`search_selection`, and `navigate_search:next|previous`. `search:<text>`
changes the engine needle without opening the overlay; an empty value stops
matching but leaves the UI alone. `search_selection` opens the overlay only
when a selection exists and preserves its untrimmed text. In the overlay,
Enter selects the next result, Shift+Enter selects the previous result, and
Escape ends search. Reopening search retains and selects the previous entry.

Drag with the left mouse button to select; double-click to select a word and
hold `Alt` while dragging for a rectangular selection. `Shift` bypasses an
application's mouse-reporting mode so local selection and scrolling remain
available. Hold `Ctrl` over an OSC 8 hyperlink or a URL/path recognized by
Ghostty's default matcher to show its pointer and underline, then release the
left button over the same tracked target without dragging to open it. Relative
file matches are resolved against the terminal's current working directory
when that target exists. When enabled, the preview shows the accepted raw
destination in a bounded, middle-elided overlay at the bottom-left of that
pane. Moving into the overlay retains the logical link hover and relocates the
overlay to the bottom-right; leaving its original guard resumes terminal hit
testing. Control and bidirectional formatting characters are escaped, invalid
UTF-8 is replaced visibly, and long byte strings are capped before layout.
When an application has captured the mouse, use
`Ctrl+Shift`: `Shift` releases the capture before the exact `Ctrl` link
modifier is matched. Middle-click follows the configured primary-paste/ignore
policy and falls back to the standard clipboard when primary selection is not
available.

## Desktop integration

Installation adds configuration-specific desktop activation metadata:

```text
${CMAKE_INSTALL_DATADIR}/applications/io.github.JingYenLoh.ghostty_qt.desktop
${CMAKE_INSTALL_DATADIR}/dbus-1/services/io.github.JingYenLoh.ghostty_qt.service
```

A Debug install appends `.Debug` to both filenames and to the process identity,
so it cannot activate a Release process. The desktop entry advertises
`DBusActivatable=true`; its compatibility `Exec` starts normally with
`--gtk-single-instance=true`. The D-Bus service instead starts a resident host
with `--gtk-single-instance=true --initial-window=false`, allowing the queued
standard activation to create exactly one window rather than a bootstrap
window plus an activated window. Executable paths are finalized from the
actual `cmake --install --prefix` value for relative GNUInstallDirs; absolute
install directories remain absolute, while `DESTDIR` remains staging-only.
An auto-started process connects to the starter bus supplied by the daemon;
ordinary launches use the desktop session bus.

The standard endpoint implements source-less `Activate(a{sv})`. It exports the
required `Open(as,a{sv})` and `ActivateAction(s,av,a{sv})` method signatures but
returns `NotSupported` until URI and action payload semantics are implemented.
`activation-token` and `desktop-startup-id` are accepted only as exact D-Bus
strings, carried with their request, and exposed as `XDG_ACTIVATION_TOKEN` and
`DESKTOP_STARTUP_ID` only during the target window's `show()` call. A direct
launcher captures and clears the same environment variables before Qt starts;
all paths clear them after presentation and strip them from every terminal
child. Other platform data is ignored. The service file deliberately uses
direct D-Bus activation: no dangling `SystemdService` is advertised before a
matching user unit and notification lifecycle exist.

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
ctest --preset dev -j8 --output-on-failure
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
application's immediate, delayed, disabled, cancelled, reloaded, and explicit
multiwindow lifetime, zero-window recreation, source-stable new-tab/new-window
working-directory/font inheritance, isolated standard D-Bus owner arbitration
and timeout behavior, starter-bus-only warm and repeated cold service
activation, CLI/config bootstrap precedence, two-process zero-window
reactivation, exact one-shot activation platform-data handoff and child
scrubbing, reentrant window/workspace creation teardown, aggregate quit
coordination, per-pane
read-only input suppression and
close protection, staged relocation of terminfo and the private config helper,
and a separate DESTDIR-staged desktop/service metadata install contract. The
real QML close dialog is also exercised headlessly; workspace tab ordering,
split layout, navigation, and zoom are covered through typed actions.
Adapter/session tests cover absolute,
relative, and selection-driven viewport movement plus select-all and endpoint
adjustment. They also verify byte-exact CSI, ESC, and Zig-literal text writes,
screen/history/mode reset semantics, OSC 8 extraction across viewport and
alternate-screen state, tracked hyperlink behavior through output, reflow,
viewport hiding, reset, and pruning, and logical-line byte mapping across soft
wraps, graphemes, and wide cells. The matcher tests run Ghostty's complete
pinned URL/path corpus against the vendored Oniguruma engine; session and pane
tests cover OSC 8 precedence, live `link-url` reload, coalesced hover lookup,
stable live-output rendering, tracked regex reflow and mutation invalidation,
byte-exact copy, relative-path opening, release-only activation, all three
`link-previews` policies, live frontend-only policy changes, left/right
relocation, and bounded safe display of arbitrary destination bytes.
Search coverage includes row/UTF-8 cell mapping, soft-wrap and hard-newline
semantics, progressive worker scans, generation cancellation, overlapping and
ASCII-case-insensitive matches, navigation/viewport alignment, action/UI
separation, color parsing, and renderer highlight precedence. Selection input
coverage includes live clear-on-typing policy, physical key traits, Kitty
encoded releases/modifiers, sequence replay exclusions, IME/preedit lifecycle,
and preservation of active drag gestures.
Interactive pointer selection and unsafe-paste dialog input are not yet fully
automated.

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
  initial frame and retains one main-text scene-graph node per visible row.
  Transient geometry still scans the visible grid on each presented update;
  compatible text-run batching and retained geometry remain CPU/allocation
  optimizations.
- No X11 backend, payload-bearing secondary-launch protocol, theme editor,
  session persistence, systemd notification integration, project icon,
  AppStream metadata, or distribution package yet. Minimal desktop and direct
  D-Bus service activation metadata is installed.
  Configuration support is limited to the documented compatibility slice;
  most Ghostty keys remain planned.
- Search is an incremental compatibility foundation rather than the upstream
  engine. The public `libghostty-vt` artifact exposes no search thread because
  Ghostty's implementation currently depends on `xev`. This frontend therefore
  scans public value snapshots cooperatively on the pane worker. Reading cold
  history through public grid references temporarily restores compressed pages,
  so the scan interleaves libghostty's bounded recompression work. The public
  API exposes flat rows rather than Ghostty's internal page formatter, so page
  boundary delimiters and some blank-cell coordinate mappings are not exact;
  highlights in an older viewport can also lag until the bottom-up scan reaches
  those rows. Terminal-data mutation restarts the active query, and only the
  current primary or alternate screen is searched; no independent result set is
  retained for the inactive screen. Cooperative yielding occurs within the
  matcher after one physical-row value snapshot has been prepared, so a
  pathological maximum-width row or exceptionally large grapheme can still
  exceed the normal slice budget. The top-right overlay is not draggable yet.
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
- Hyperlink interaction covers explicit OSC 8 destinations and the built-in
  default matcher controlled by `link-url`, including the configured preview
  policy. User-defined `link` expressions and arbitrary link actions remain
  planned because the pinned Ghostty `RepeatableLink.parseCLI` returns
  `error.NotImplemented`. Public
  `libghostty-vt` exposes a hyperlink URI but not its OSC 8 ID, so visible OSC
  8 links with the same destination can be underlined as one group even when
  their IDs differ. Pathological logical lines beyond 131,072 cells or 4 MiB,
  and regex searches that exhaust Ghostty's bounded retry budget, fail closed.
- Hyperlink opening adapts absolute paths to local-file `QUrl` values and parses
  other destinations in Qt's strict encoded mode. A regex-detected relative
  path becomes a local-file URL only when the terminal has a current directory
  and the resolved target exists. Malformed or NUL-containing explicit
  destinations remain available to `copy_url_to_clipboard` but are not sent to
  the desktop opener. URI-scheme handling after validation belongs to Qt and
  the configured desktop services.
- Terminal-initiated clipboard writes are denied. User-initiated copy and paste
  are supported; styled HTML/VT clipboard formats are not yet emitted.
- Selection-dependent `performable` bindings use asynchronously reconciled UI
  state. Immediate select-all chains retain worker order, but a separate key
  event can still race a blank select-all completion or terminal-driven
  selection change; worker-authoritative performability remains planned.
- The first appearance slice covers the full palette, selection, cursor,
  bold, faint, and split appearance settings documented above. Dynamic
  light/dark theme switching, font fallback lists, palette
  generation/harmonization, and the rest of Ghostty's appearance model remain
  planned. The pinned
  `+show-config` text boundary does not expose the derived palette and explicit
  entry mask needed to reproduce `palette-generate` or `palette-harmonious`
  exactly.
- Automated startup testing uses Qt's software scene-graph backend. The GPU/RHI
  renderer still needs interactive visual qualification on real Wayland
  compositors and driver combinations.
