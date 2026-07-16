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
| Terminal engine | Zig-built static `libghostty-vt` through its C API | VT parsing, terminal state, render-state iteration, selection, and key/mouse/paste encoding. |
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
              -> libghostty-vt terminal/render/input handles
              -> PTY master <-> child process group
```

`TerminalWorkspace` is a C++ `QQuickItem` exposed to QML. QML owns only the
application chrome and dialogs. Each tab owns a recursive binary tree whose
leaves are `TerminalPane` objects; internal nodes describe horizontal or
vertical 50/50 splits. Only the current tab's panes are visible. The active
pane supplies the tab title and receives toolbar and directional-focus actions.

Closing a live pane, tab, or window requests confirmation. Destruction sends
`SIGHUP` to the child's process group, allows a two-second grace period, and
uses `SIGKILL` if the group does not exit.

## Output path

1. `SessionWorker` creates a Ghostty terminal and render-state iterators, then
   starts the command with `forkpty`.
2. A nonblocking `QSocketNotifier` drains the PTY master and sends bytes to
   `ghostty_terminal_vt_write`.
3. Ghostty applies VT state changes and reports deferred effects such as title,
   working directory, and bell notifications.
4. Screen updates are coalesced on an 8 ms single-shot timer. The worker walks
   Ghostty's render-state rows and cells into a value-only `TerminalFrame`:
   grapheme strings, wide-cell head/tail metadata, resolved colors and styles,
   selection flags, cursor state, and scrollbar values.
5. A queued Qt signal copies the frame across the thread boundary. The pane
   replaces its latest snapshot under a mutex and schedules a scene-graph
   update.
6. `TerminalPane::updatePaintNode()` repopulates its retained render-node root. Public
   `QSGTextNode` objects use `QtRendering`, which stores distance-field glyphs
   in GPU atlases on hardware RHI backends. Batched colored geometry draws cell
   backgrounds, selections, cursor shapes, text decorations, overlays, and the
   scrollbar.
7. Each nonempty cell is shaped with `QTextLayout` and placed at an explicit
   grid coordinate before its glyph data is added to a text node. This avoids
   fallback-font and wide-cell advances shifting later cells.

No Ghostty handle crosses into the UI/render side. The value snapshot makes
ownership explicit and decouples PTY parsing from rendering, at the cost of
copying a full grid for each published frame.

Renderer-v1 uses Qt's public scene-graph API throughout: text nodes supply the
GPU glyph path and colored `QSGGeometryNode` batches supply solid primitives.
No intermediate raster-image upload sits between the frame and the scene
graph. The current implementation retains the root but recreates its children
for each update and performs a `QTextLayout` per rendered cell, so shaping,
node construction, and full-frame snapshot copying remain CPU-side performance
targets.

## Input path

Qt events travel in the opposite direction through value types and queued
signals:

- Key press/release events are mapped to Ghostty key events. The Ghostty key
  encoder reads the current terminal modes before producing PTY bytes.
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
TERMINFO=<build tree>/share/terminfo
COLORTERM=truecolor
TERM_PROGRAM=ghostty-qt
TERM_PROGRAM_VERSION=<project version>
```

When a child exits, its final PTY output is drained and one last frame is
published. A normal, non-held pane then closes automatically. A held or failed
pane remains visible with a status message.

## Build integration

The source tree expects Ghostty commit
`c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3`. CMake validates the revision when
the checkout has Git metadata and rejects a mismatch unless upgrade work
explicitly enables `GHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY`.

Ghostty's CMake wrapper invokes Zig and exports `ghostty-vt-static`. The project
checks `zig version` at configure time and accepts only `0.15.2`, matching the
pinned Ghostty source. The C++ executable links that static target and Linux
`libutil`.

The build also runs a small Zig generator against Ghostty's terminfo source and
compiles the result with `tic -x`. The generated database is a dependency of the
application and PTY integration test. Children point directly at the selected
preset's database, so no system terminfo installation is required for a build-
tree run.

Ghostty places generated artifacts in its source-tree `zig-out`, shared by the
developer and release CMake trees. Those presets must not build concurrently.
The current install rules are preliminary: runtime terminfo lookup is compiled
with the build-tree path, so the project does not yet claim a relocatable binary
package.

Qt's `emit` macro is disabled with `QT_NO_KEYWORDS` because the public Ghostty C
API legitimately contains struct fields named `emit`.

## Test boundaries

The CTest suite has five layers:

- `launch-options` validates defaults, accepted values, and invalid CLI input.
- `ghostty-smoke` exercises terminal parsing/render-state iteration, CJK wide
  cells, key and 1002 mouse-drag encoding, bracketed paste, and terminal query
  callbacks directly through the C API.
- `session-worker` starts real PTY children and verifies DA replies, bracketed
  paste fence bytes, final output draining, and process exit.
- `terminal-pane-render` renders a delayed PTY frame offscreen and verifies the
  initial placeholder contents were replaced rather than retained across
  updates.
- `application-lifecycle` starts the complete QML application on Qt's offscreen
  software backend, verifies a short-lived child closes the window cleanly,
  and fails on QML binding-loop diagnostics.

Interactive selection/tab/split and confirmation-button behavior are not fully
automated yet. The offscreen tests validate QML startup and scene-graph frame
replacement in a headless environment, but they do not validate the hardware
RHI path. `GHOSTTY_QT_ALLOW_NON_WAYLAND=1` is a test escape hatch rather than a
supported runtime configuration; GPU output must also be checked interactively
in a real Wayland session.

## Deliberate renderer-v1 limits

- Full-grid value snapshots, child-node rebuilding, and per-cell `QTextLayout`
  calls trade CPU performance for a simple, auditable thread boundary and exact
  grid placement. Dirty-row reuse and larger compatible text runs remain future
  optimizations.
- Text uses Qt's GPU distance-field glyph atlas on hardware RHI backends, but
  there is no ligature shaping across terminal cells, color-emoji pipeline, or
  Kitty graphics/inline-image renderer.
- Split ratios are fixed; no divider interaction is implemented.
- Theme/config loading, search, hyperlinks, multi-window operation, saved
  sessions, and a relocatable packaging strategy remain future work.
