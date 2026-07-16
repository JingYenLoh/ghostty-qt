# Architecture

## Scope and stack

`ghostty-qt` is intentionally Linux- and Wayland-only. Its first milestone is a
usable terminal core with tabs and splits, while keeping the boundary between
the Qt application and Ghostty's terminal engine small enough to evolve.

| Layer | Technology | Responsibility |
| --- | --- | --- |
| Window and chrome | Qt Quick, QML, Quick Controls 2 | Window, tab strip, toolbar, and confirmation dialogs. |
| Terminal item and workspace | C++20, Qt Quick | Scene-graph rendering, input events, focus, tabs, and a recursive split tree. |
| Session orchestration | Qt Core/Gui on a dedicated `QThread` per pane | PTY I/O, child lifecycle, immutable frame snapshots, and queued UI communication. |
| Ghostty adapter | C++20 value-type boundary | Contains the `libghostty-vt` C API and translates terminal, render, input, selection, and deferred-effect operations. |
| Terminal engine | Zig-built static `libghostty-vt` through its C API | VT parsing, terminal state, render-state iteration, selection, and key/mouse/paste encoding. |
| Configuration | Qt file-watching service plus a helper process | Uses the exact pinned Ghostty application parser, then exposes only selected typed values to the Qt application. |
| Application keybindings | C++20 plus Qt D-Bus | Routes app-scoped and all-surface actions, and owns the Linux XDG Global Shortcuts portal session. |
| Process boundary | Linux `forkpty`, nonblocking file descriptors, `QSocketNotifier` | Shell or command execution and byte transport. |
| Build | CMake/Ninja plus Zig 0.15.2 | Qt application build, pinned Ghostty build, generated terminfo, and tests. |

C++ is the host language because Qt's native API and QML type system are C++,
while `libghostty-vt` already exposes a C ABI. Zig remains responsible for
building Ghostty; adding a Zig-to-Qt binding layer would not simplify the
runtime boundary for this MVP.

## Runtime structure

Each terminal pane has the following ownership and data flow:

```text
Main.qml
  -> TerminalWorkspace (UI thread, recursive tabs/splits)
     -> TerminalPane (QQuickItem with scene-graph contents)
        -> TerminalController (UI thread)
           <queued signals>
           -> SessionWorker (one dedicated QThread)
              -> GhosttyVtAdapter
                 -> libghostty-vt terminal/render/input handles
              -> PTY master <-> child process group

GhosttyConfigService (UI thread)
  -> ghostty-qt-config-helper (short-lived child process)
     -> pinned ghostty-internal configuration/CLI implementation

GhosttyApplicationKeybindings (UI thread, process lifetime)
  -> root application-action pre-pass
  -> all/global action-major fanout -> registered TerminalWorkspace instances
  -> XDG Global Shortcuts portal (session D-Bus)
```

`TerminalWorkspace` is a C++ `QQuickItem` exposed to QML. QML owns only the
application chrome and dialogs. Each tab owns a recursive binary tree whose
leaves are `TerminalPane` objects; internal nodes describe horizontal or
vertical 50/50 splits. Only the current tab's panes are visible. The active
pane supplies the tab title and receives toolbar and directional-focus actions.

Tabs and panes have monotonically assigned `TabId` and `PaneId` values. The
workspace resolves those identities at execution time instead of retaining
vector rows or raw pane pointers across deferred operations. A
`QAbstractListModel` publishes tab identity, title, active pane, working
directory, running state, attention, progress, and read-only roles to QML. The
current tab strip consumes that model; several roles are foundations for later
parity work rather than user-visible features today.

Workspace commands pass through a typed `WorkspaceActionDispatcher` with an
explicit tab/pane context. Keyboard, pane, and QML entry points can therefore
converge on the same action vocabulary as more Ghostty keybindings are added.
Pending close and unsafe-paste operations retain stable IDs, so a model row
moving before confirmation cannot redirect the operation to another pane or
tab. Broad unsafe paste batches every stable target behind one confirmation;
broad close converges on one confirmed shutdown request per workspace.

Close policy tracks the live child separately from active foreground work. For
an interactive shell, `tcgetpgrp` detects jobs in a separate foreground process
group, with a short conservative latch around command submission; an explicitly
launched program is active for its whole lifetime. `always` protects any live
child, and neither mode prompts after exit. The current fallback cannot detect
long-running shell builtins because they remain in the shell's own process
group; exact `confirm-close-surface=true` behavior requires Ghostty semantic
prompt state plus installed shell integration and remains a tracked parity
item. Pending dialogs are re-evaluated when state changes. Destruction sends
`SIGHUP` to the child's process group, allows a two-second grace period, and
uses `SIGKILL` if the group does not exit; workspace/tab teardown starts all
pane shutdowns first so grace periods overlap.

## Output path

1. `SessionWorker` creates a `GhosttyVtAdapter`, which owns the Ghostty terminal
   and render-state handles, then starts the command with `forkpty`.
2. A nonblocking `QSocketNotifier` drains the PTY master and sends bytes to
   the adapter's VT-write operation.
3. Ghostty applies VT state changes and reports deferred effects such as title,
   working directory, and bell notifications.
4. Screen updates are coalesced on an 8 ms single-shot timer. First frames,
   resize, viewport scroll, and Ghostty full-dirty states produce a value-only
   full-grid update. Ordinary output copies only Ghostty's indexed dirty rows;
   colors, the effective 256-entry palette, cursor state, and scrollbar
   metadata carry independent change flags.
5. A queued Qt signal copies the update across the thread boundary. The pane
   validates and transactionally merges it into its retained frame under a
   mutex, then schedules a scene-graph update. Dirty state is cleared only
   after the adapter successfully copies the complete update.
6. `TerminalPane::updatePaintNode()` repopulates its retained render-node root. Public
   `QSGTextNode` objects use `QtRendering`, which stores distance-field glyphs
   in GPU atlases on hardware RHI backends. Color-batched solid geometry draws cell
   backgrounds, selections, cursor shapes, text decorations, overlays, and the
   scrollbar. Cell values retain foreground provenance and bold, faint,
   inverse, invisible, underline, strike-through, overline, and text-blink
   attributes so frontend-only appearance rules do not have to be flattened at
   the worker boundary.
7. Each nonempty cell is shaped with `QTextLayout` and placed at an explicit
   grid coordinate before its glyph data is added to a text node. This avoids
   fallback-font and wide-cell advances shifting later cells.

The application-facing adapter header contains only Qt and project value
types; the Ghostty C header and every Ghostty handle remain in its private
implementation. No Ghostty handle crosses into `SessionWorker` or the
UI/render side. The adapter returns value updates and deferred effects, making
ownership explicit and localizing future upstream C API changes. A full-grid
fallback keeps resize and viewport changes simple while ordinary output avoids
copying unchanged rows between threads.

Renderer-v1 uses Qt's public scene-graph API throughout: text nodes supply the
GPU glyph path and one vertex-colored `QSGGeometryNode` per painter layer
supplies solid primitives on RHI backends. Qt's software adaptation does not
render that public vertex-color material, so the test/fallback path uses
`QSGSimpleRectNode` groups for correctness.
No intermediate raster-image upload sits between the frame and the scene
graph. The current implementation retains the root but recreates its children
for each update and performs a `QTextLayout` per rendered cell, so shaping,
node construction, and full-frame snapshot copying remain CPU-side performance
targets.

The renderer resolves configured selection and cursor cell-relative aliases
against each cell's visual colors, applies Ghostty's bold palette/direct-color
rules before inverse presentation, and applies faint opacity to glyphs and
decorations without dimming the cell background. It draws block, bar,
underline, and hollow cursors plus single, double, curly, dotted, and dashed
underlines. Text-blink state is retained in the value model but intentionally
does not drive an animation because the pinned Ghostty generic renderer also
leaves blinking text visible; cursor blink remains an independent animation.

## Input path

Qt events travel in the opposite direction through value types and queued
signals:

- Key press/release events are mapped to project value types. The adapter's
  Ghostty key encoder reads the current terminal modes before producing PTY
  bytes.
- IME commit strings use the same encoder path; preedit text remains a local UI
  overlay until committed.
- Mouse events use Ghostty's mouse encoder when an application enables mouse
  tracking. Holding `Shift` retains local selection and scrollback behavior.
- Focus changes are encoded only when the terminal requests focus reporting.
- Paste uses Ghostty's safe-paste check and bracketed-paste encoder. Unsafe text
  is held in `TerminalWorkspace` until the QML dialog confirms it.
- Selection is stored in Ghostty's terminal model and formatted by Ghostty for
  clipboard copy. Libghostty's tracked selection-gesture state keeps the drag
  anchor stable across output, scrolling, and resize. OSC-driven clipboard
  writes from terminal applications are denied by the host callback.

Resize starts in `TerminalPane`: font metrics and item geometry determine rows,
columns, cell pixels, and surface pixels. The worker resizes both Ghostty's
terminal and the kernel PTY with `TIOCSWINSZ`.

After output becomes idle, the worker runs libghostty's bounded incremental
scrollback compression until the current pass completes. Compression and all
other terminal access remain serialized on the pane's worker thread.

## Session and environment

The first pane receives the parsed command-line options. Without a command,
the worker starts executable `$SHELL` or `/bin/sh`. A new tab starts the default
shell in the original working directory. A split starts the default shell using
the source pane's latest reported directory, font, and font size.

The child inherits the host environment with these terminal-specific values:

```text
TERM=xterm-ghostty
TERMINFO=<resolved private database>
COLORTERM=truecolor
TERM_PROGRAM=ghostty-qt
TERM_PROGRAM_VERSION=<project version>
```

When a child exits, its final PTY output is drained and one last frame is
published. A normal, non-held pane then closes automatically. A held or failed
pane remains visible with a status message.

## Configuration boundary

Ghostty's application configuration API is not part of the stable
`libghostty-vt` surface. The Qt executable therefore does not link or retain
handles from that API. Instead, `ghostty-qt-config-helper` links the pinned
`ghostty-internal` shared library. It preserves Ghostty's existing command-line
actions and adds one project-private structured binding export. For each load
the Qt-side process adapter invokes, in order:

```text
+validate-config
+show-config --default
+show-config
+show-keybinds-json
+validate-config
+show-config
+show-keybinds-json
```

The helper runs with the selected `XDG_CONFIG_HOME`, so Ghostty itself owns
standard-file discovery, legacy/preferred-file precedence, include handling,
validation, defaults, and canonical formatting. The final two queries must
byte-match the first current-config and binding outputs, preventing a valid
A-to-B edit from publishing a mixed snapshot. The adapter merges the default
and changed-value output into a value-only `GhosttyConfigSnapshot`. This keeps
the unstable application API and all of its state outside the long-lived Qt
process while avoiding a second parser.

`GhosttyConfigService` watches the legacy `ghostty/config`, preferred
`ghostty/config.ghostty`, their nearest existing directories, existing include
files, and directories that can create missing optional includes. A 75 ms timer
coalesces file and atomic-replacement notifications. The initial load is
synchronous before the UI exists; watched and action-requested reloads run on a
dedicated one-thread pool so helper timeouts cannot freeze the GUI thread or
hold Qt's global pool open at shutdown. Load generations prevent an older async
result from replacing a newer synchronous load. Post-query validation rejects
a file that became invalid during extraction, consistency queries reject a
valid concurrent edit, successful helper
warnings enter the typed diagnostic list, and failed loads retry periodically
to discover newly created required includes. Failure still leaves the last
good snapshot active when one exists; a successful changed snapshot is applied
to the workspace.

The current typed compatibility slice contains `font-family`, `font-size`, the
appearance keys listed below, `scrollback-limit`, `confirm-close-surface`,
`config-file`, and a versioned dump of the finalized keybinding sets.
Appearance crosses threads as a
value-only `TerminalAppearance`: terminal foreground/background, all 256
palette defaults, selection colors, cursor color/style/blink/opacity/text,
bold-color, and faint-opacity. Fixed colors and Ghostty's cell-foreground and
cell-background aliases remain distinct until the renderer has the target
cell. Only the first configured font family is rendered. Explicit font and
scrollback CLI options retain precedence.

Live reload updates font and appearance on existing panes without overriding a
pane's manual font zoom. Palette and fixed cursor defaults are updated through
`libghostty-vt`, which preserves terminal-originated OSC 4/OSC 12 overrides;
OSC 104/OSC 112 reset to the newest configured defaults. Likewise, an active
DECSCUSR cursor style survives a config reload and its reset selects the newest
configured style. Selection, cursor aliases/opacity/text, bold-color, and
faint-opacity are frontend render policy and therefore update without mutating
terminal-originated state. Close confirmation policy also updates live.

Two parser/API boundaries remain explicit. A null `cursor-style-blink` maps to
Ghostty's initial blinking default, but the public `libghostty-vt` setter takes
only a boolean; it cannot represent the upstream tri-state in which explicit
true/false ignores DEC mode 12. The pinned `+show-config` text output also
precedes derived palette generation and loses Ghostty's explicit-entry mask,
so `palette-generate` and `palette-harmonious` remain planned rather than being
approximated. Static themes can contribute canonical appearance values through
the pinned parser, while dynamic light/dark theme selection is not implemented.

Because the pinned terminal API cannot resize an existing scrollback
allocation, the byte-valued Ghostty limit applies when a pane is created; a
reload affects later panes. `config-file` contributes parser input and watcher
paths rather than a direct renderer setting.

The pinned terminal allocation limit is byte-valued. Ghostty's
`scrollback-limit` therefore passes through exactly. The older
`--scrollback-lines` CLI remains accepted through an explicit estimate of
`max(256, columns * 16)` bytes per requested row, using the initial terminal
width and saturating arithmetic; it is a capacity estimate, not an exact row
guarantee because Ghostty pages also store styles and grapheme metadata.

## Keybinding compatibility boundary

The config helper exposes a project-private JSON v1 dump of Ghostty's finalized
binding sets, after defaults, includes, `clear`, overrides, chains, and
`unbind` have been resolved by the pinned Zig implementation. It retains full
root sequences, named tables, physical/Unicode/catch-all triggers, canonical
action chains, and every binding flag. The C++ parser is strict and
transactional: an unknown schema or malformed dump rejects the reload without
replacing the last-good snapshot.

Each `TerminalPane` builds node-indexed tries for the generation's root and
named tables and owns its traversal and table-stack state. Sharing those
immutable tries between panes is a later allocation optimization. Outside an
active sequence, lookup walks the newest active table outward and then the
root. A one-shot top table is popped as soon as it supplies a match, including
a leader, catch-all, or performable binding. Lookup prioritizes
physical identity, then event Unicode, then the unshifted codepoint, then
modifier-specific and bare catch-all entries at every depth. On
Linux/Wayland, native XKB scan codes keep physical triggers and libghostty's
physical-key encoding layout-independent, while distinguishing top-row/keypad
and left/right modifier locations. Qt does not expose the compositor keymap's
unmodified layout level through `QKeyEvent`, so the fallback unshifted
codepoint remains US-layout-oriented for shifted punctuation. Reading Wayland
keymap state directly is a later input-compatibility slice.

Sequence leader presses are encoded immediately on the session thread and held
as bytes under a generation token. A consumed match drops them; an invalid,
unconsumed, or unavailable performable match flushes the prefix and current key
atomically; `end_key_sequence` flushes only the leaders. This preserves the VT
mode that existed at each leader press and prevents reload or stale queued
operations from leaking input. Supported actions then pass through
`GhosttyActionCatalog` and the same typed
workspace dispatcher used by QML controls; pane-local copy, paste, zoom,
scroll, and reload actions use their terminal operations directly.

`GhosttyApplicationKeybindings` performs root app-scoped leaves before the
focused pane lookup, matching Ghostty's app/surface split while leaving leaders
and mixed-scope chains to the pane. A pane that matches `all` or `global`
forwards the chain to that process controller. It executes app actions once and
surface actions over a stable pane snapshot, action-major across the chain;
`unconsumed` and `performable` do not alter broad-binding consumption.

On Linux, eligible root `global` bindings are registered asynchronously through
the XDG Global Shortcuts portal using Qt D-Bus. Request response subscriptions
are installed before calls, and config generations reject stale create/bind
callbacks. Reload closes the previous session before registration. Matching the
pinned GTK frontend, only direct root bindings with one action are portal
eligible; sequences, catch-all triggers, table entries, and action chains are
diagnosed and skipped. Portal failure is nonfatal and never falls back to a
focus-only Qt shortcut.

## Build integration

The source tree expects Ghostty commit
`c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3`. CMake validates the revision when
the checkout has Git metadata and rejects a mismatch unless upgrade work
explicitly enables `GHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY`.

Ghostty's CMake wrapper invokes Zig and exports `ghostty-vt-static`. The project
checks `zig version` at configure time and accepts only `0.15.2`, matching the
pinned Ghostty source. The C++ executable links that static target and Linux
`libutil`.

With `GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG=ON` (the default), CMake also asks the
pinned Ghostty build for `ghostty-internal.so` with its application runtime
disabled. A revision-scoped source shadow overlays the one private structured
binding export without modifying the pinned submodule; shadow creation and the
shared Zig install transaction are lock-protected across build trees. Its Zig
artifacts live in ignored project-local
`.cache/ghostty-internal` and `.cache/zig-global` directories. Only the small
config helper links this private shared library. The installed helper is beside
the main executable and resolves the library from a relative private
`${CMAKE_INSTALL_LIBDIR}/ghostty-qt` directory; disabling the option omits this
configuration path entirely.

The build also runs a small Zig generator against Ghostty's terminfo source and
compiles the result with `tic -x`. The generated database is a dependency of the
application and PTY integration test. A build-tree run finds `share/terminfo`
beside the executable. An installed executable first resolves its private
`${CMAKE_INSTALL_DATADIR}/ghostty-qt/terminfo` directory using only a relative
path, so moving the complete installation prefix does not invalidate it. The
`GHOSTTY_QT_TERMINFO` environment variable is an authoritative diagnostic
override. No system terminfo installation is required.

Ghostty places generated artifacts in its source-tree `zig-out`, shared by the
developer and release CMake trees. Those presets must not build concurrently.
The staged relocation test installs into a temporary prefix, moves the prefix,
and runs a Qt Core-only probe from the moved `bin` directory to verify that it
selects the moved private database. Production desktop metadata and packaging
remain separate work.

Qt's `emit` macro is disabled with `QT_NO_KEYWORDS` because the public Ghostty C
API legitimately contains struct fields named `emit`.

## Parity contract

`docs/ghostty-parity.json` pins the same Ghostty revision as CMake, records the
Linux/Wayland/Qt scope, and inventories upstream configuration keys,
keybinding actions, and CLI actions with explicit status and scope labels. The
target is to preserve portable and Linux Ghostty configuration names and
semantics while mapping meaningful GTK frontend behavior onto Qt. GTK-only
presentation/debugging internals, X11, macOS, iOS, and FreeBSD behavior are not
part of this frontend's parity target.

`scripts/check-ghostty-parity.py` re-extracts those inventories from the pinned
source and rejects revision, schema, ordering, or inventory drift. This keeps
an upstream snapshot update from silently adding untracked parity work. The
contract remains conservative: only the typed configuration slice is marked as
partial or supported, while the other upstream keys stay explicitly planned.

## Test boundaries

The default CTest suite has seventeen layers:

- `launch-options` validates defaults, accepted values, invalid CLI input,
  typed config and appearance overlays, CLI font precedence, scrollback units,
  and close modes.
- `ghostty-smoke` exercises terminal parsing/render-state iteration, CJK wide
  cells, key and 1002 mouse-drag encoding, bracketed paste, and terminal query
  callbacks directly through the C API.
- `ghostty-vt-adapter` verifies the application-facing boundary renders value
  snapshots, carries style provenance and effective palette state, preserves
  OSC and DECSCUSR overrides across appearance reloads, reports
  title/directory/bell effects, handles terminal callbacks, and encodes paste,
  focus, and key input using terminal modes.
- `session-worker` starts real PTY children and verifies DA replies, bracketed
  paste fence bytes, staged sequence ordering and stage-time VT modes, final
  output draining, process exit, explicit-program activity, and an interactive
  shell's idle/job/idle foreground transitions.
- `terminal-workspace` verifies that active programs request confirmation,
  idle shells follow `true` versus `always`, pending quit resolves on process
  exit, and approval is emitted once.
- `workspace-foundation` verifies stable tab identity after row removal, tab
  model role updates, and typed action context dispatch.
- `ghostty-action-catalog` verifies the supported subset of pinned Ghostty
  action-string parsing and deterministic malformed/unsupported results.
- `ghostty-keybind-set` verifies delimiter edge cases, native physical-key
  locations, shifted/unshifted Unicode matching, shared-prefix sequences,
  catch-all priority and recovery, local/broad flags, action chains, named-table
  precedence and one-shot state, independent pane state, and Linux defaults.
- `ghostty-global-shortcut-portal` verifies XDG trigger conversion, registry
  eligibility and collisions, response-before-reply races, activation routing,
  reload cleanup, and stale callback rejection on a private D-Bus daemon.
- `ghostty-config-service` verifies standard paths, file/directory and include
  watches, atomic replacement, debounce, and retention of the last good value.
- `ghostty-config-process-loader` verifies canonical and structured snapshot
  parsing, the validation/default/current/keybinding/post-validation protocol,
  deterministic process failure paths, warning preservation, and real-parser
  `clear`/`unbind` resolution.
- `ghostty-config-helper-smoke` runs `+validate-config` through the helper and
  exact pinned Ghostty parser built for the application.
- `terminal-pane-render` renders frames offscreen, verifies the initial
  placeholder is replaced plus selection/cursor/text appearance, and exercises
  sequence consume/replay, performability, release suppression, and reload
  cancellation through a real PTY-backed pane.
- `application-lifecycle` starts the complete QML application on Qt's offscreen
  software backend, verifies a short-lived child closes the window cleanly,
  and fails on QML binding-loop diagnostics.
- `application-close-dialog` opens and accepts the real QML close confirmation
  around a live child, failing on binding loops or shutdown regressions.
- `ghostty-parity-manifest` checks the pinned revision and upstream-derived
  configuration, keybinding-action, and CLI-action inventories.
- `terminfo-relocatable-install` stages and moves an installation, executes the
  relocated private config helper, verifies runtime terminfo lookup, and checks
  valid and invalid explicit overrides.

Interactive selection/tab/split and unsafe-paste confirmation behavior are not
fully automated yet. The offscreen tests validate QML startup, close-dialog
shutdown, and scene-graph frame replacement in a headless environment, but they
do not validate the hardware RHI path. `GHOSTTY_QT_ALLOW_NON_WAYLAND=1` is a
test escape hatch rather than a
supported runtime configuration; GPU output must also be checked interactively
in a real Wayland session.

## Deliberate renderer-v1 limits

- Dirty-row value updates keep the thread boundary small for ordinary output,
  while child-node rebuilding and per-cell `QTextLayout` calls still trade CPU
  performance for an auditable renderer and exact grid placement. Persistent
  row nodes and larger compatible text runs remain future optimizations.
- Text uses Qt's GPU distance-field glyph atlas on hardware RHI backends, but
  there is no ligature shaping across terminal cells, color-emoji pipeline, or
  Kitty graphics/inline-image renderer.
- Public `libghostty-vt` cannot preserve configured cursor-blink tri-state
  precedence over DEC mode 12, and the text config dump cannot expose the
  post-generation palette mask. Those cases remain explicitly partial/planned
  in the parity ledger.
- Split ratios are fixed; no divider interaction is implemented.
- Configuration beyond the documented typed slice, unsupported keybinding
  actions, search, hyperlinks, multi-window operation, saved sessions, and
  production packaging remain future work.
