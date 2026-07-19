# Architecture

## Scope and stack

`ghostty-qt` is intentionally Linux- and Wayland-only. Its first milestone is a
usable terminal core with tabs and splits, while keeping the boundary between
the Qt application and Ghostty's terminal engine small enough to evolve.

| Layer | Technology | Responsibility |
| --- | --- | --- |
| Window and chrome | Qt Quick, QML, Quick Controls 2 | Window, tab strip, toolbar, and confirmation dialogs. |
| Terminal item and workspace | C++23, Qt Quick | Scene-graph rendering, input events, focus, tabs, and a recursive split tree. |
| Session orchestration | Qt Core/Gui on a dedicated `QThread` per pane | PTY I/O, child lifecycle, immutable frame snapshots, and queued UI communication. |
| Ghostty adapter | C++23 value-type boundary | Contains the `libghostty-vt` C API and translates terminal, render, input, selection, search-snapshot, and deferred-effect operations. |
| Terminal engine | Zig-built static `libghostty-vt` through its C API | VT parsing, terminal state, render-state iteration, selection, and key/mouse/paste encoding. |
| Configuration | Qt file-watching service plus a helper process | Uses the exact pinned Ghostty application parser, then exposes only selected typed values to the Qt application. |
| Application keybindings | C++23 plus Qt D-Bus | Routes app-scoped and all-surface actions, and owns the Linux XDG Global Shortcuts portal session. |
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

`LaunchOptions` remains on the application, workspace, and pane side because it
also owns font policy, keybindings, close behavior, link previews, the effective
working-directory and future-surface inheritance policies, and a compact
`SplitAppearance` value containing unfocused-pane opacity/fill and the optional
divider color. Split appearance stays
frontend-owned and never crosses the session-worker boundary. Before a
session starts, the pane projects `LaunchOptions` to
`TerminalSessionLaunchOptions`, containing only the child process, scrollback,
appearance, and URL-matching state owned by the session. Live reloads cross the
queued controller/worker boundary as `TerminalSessionRuntimeOptions`, which is
limited to appearance and URL matching. Working directory, program, hold, and
the existing terminal's scrollback allocation are launch-only by construction.
A separate launch bit distinguishes process-cwd inheritance from a concrete
CLI, config, or OSC-derived directory; this prevents a display path from
silently turning `inherit` into a `chdir` and `PWD` rewrite.
A pane reload first compares its pane-owned value state, then compares the
projected worker runtime state; an identical update does no work, and a
frontend-only change does not enqueue a worker request.

`TerminalWorkspace` is a C++ `QQuickItem` exposed to QML. QML owns only the
application chrome and dialogs. Each tab owns a recursive binary tree whose
leaves are `TerminalPane` objects; internal nodes describe horizontal or
vertical splits with mutable ratios. The workspace supports cyclic in-order and
wrapped spatial focus, nearest-axis keybinding resizing, exact-divider pointer
dragging, axis-aware equalization, and a per-tab zoomed leaf. Every internal
node has a stable split identity. Workspace-owned pointer handles resolve that
identity on each event instead of retaining tree pointers that pane closure can
invalidate. Their hit rectangles are exactly the existing two-logical-pixel
layout gaps, so they never cover a terminal cell; adjacent half-open rectangles
also make nested T-junction ownership deterministic without stacking tricks.
An optional configured RGB color is painted by one public
`QSGSimpleRectNode` per visible handle; an unset value creates no scene-graph
node and exposes the ordinary Qt/QML gap color. Color reloads update existing
handles in place, while newly created handles inherit the current workspace
value. Drag ratios remain unitless, clamp to `[0, 1]`, and relayout only after
the floored divider position changes. The handles accept no keyboard focus, so
active-pane and search-overlay focus survive a drag. Moving the workspace to a
different Qt Quick scene destroys the old scene's handles synchronously to
release any delivery-agent grab, then recreates them from the stable split IDs.

Zoom changes only presentation: the complete tree, ratios, PTYs, and logical
geometry remain intact, while divider handles are absent. Only the current
tab's panes and dividers are exposed. The active pane supplies the tab title
and receives toolbar and directional-focus actions. A new split can be placed
on any side of its source; left/up insert the new leaf before the source and
right/down insert it after. The omitted or explicit `auto` direction compares
the source pane's effective surface-pixel width and height, choosing right only
when width is greater and down otherwise. Every split focuses the new leaf and
clears split zoom.

Tabs and panes have monotonically assigned `TabId` and `PaneId` values. The
workspace resolves those identities at execution time instead of retaining
vector rows or raw pane pointers across deferred operations. A
`QAbstractListModel` publishes tab identity, title, active pane, working
directory, running state, zoom, attention, progress, and read-only roles to QML. The
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
   colors, the effective 256-entry palette, cursor state, scrollbar metadata,
   and each cell's OSC 8 presence bit cross as values. Each published update
   also carries the worker's current terminal-content revision.
5. A queued Qt signal copies the update across the thread boundary. The pane
   validates and transactionally merges it into its retained frame under a
   mutex, then schedules a scene-graph update. Dirty state is cleared only
   after the adapter successfully copies the complete update.
6. `TerminalPane::updatePaintNode()` keeps fixed before-text/main-text/after-text
   groups followed by one persistent full-pane unfocused-split rectangle. Main
   terminal text is retained in one public `QSGTextNode` per visible row;
   accepted row epochs rebuild only changed rows, while font, geometry,
   appearance, palette, search, and frame-shape changes rebuild the complete
   text layer. Old and new block-cursor rows are rebuilt when its text override
   changes. The row nodes use `QtRendering`, which stores distance-field glyphs
   in GPU atlases on hardware RHI backends. The final rectangle keeps an empty
   extent while inactive and updates without scene-graph allocation. Dimming
   state is absent from the retained text-state key. Existing focus-driven
   block-cursor changes keep their targeted row rebuilds, while search
   decoration changes retain their full text-state invalidation.
7. Color-batched transient geometry continues to draw cell backgrounds,
   selections, search results, cursor shapes, text decorations, overlays, and
   the scrollbar in the same global painter order. Each nonempty cell in a
   rebuilt row is shaped with `QTextLayout` and placed at an explicit grid
   coordinate. This avoids fallback-font and wide-cell advances shifting later
   cells. Cell values retain foreground provenance and bold, faint, inverse,
   invisible, underline, strike-through, overline, and text-blink attributes so
   frontend-only appearance rules do not have to be flattened at the worker
   boundary.

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
graph. Qt's implicitly shared frame snapshot is normally an O(1) reference-count
operation rather than a deep cell copy. The renderer still scans visible cells
and rebuilds transient solid geometry on every presented update. During
ordinary sparse updates, it shapes text and recreates glyph data only for rows
whose persistent epochs or derived block-cursor text state changed; global
text-state changes rebuild the complete text layer. Larger compatible text runs
and retained geometry remain possible CPU-side optimizations.

The renderer resolves configured selection, search, and cursor cell-relative
aliases against each cell's visual colors, applies Ghostty's bold
palette/direct-color rules before inverse presentation, and applies faint
opacity to glyphs and decorations without dimming the cell background. Normal
terminal selection has priority over the selected search result, which has
priority over candidate search results; a wide cell's spacer inherits the
owning cell's search layer. The renderer draws block, bar, underline, and
hollow cursors plus single, double, curly, dotted, and dashed underlines.
Text-blink state is retained in the value model but intentionally does not
drive an animation because the pinned Ghostty generic renderer also leaves
blinking text visible; cursor blink remains an independent animation.

## Input path

Qt events travel in the opposite direction through value types and queued
signals:

- Key press/release events are mapped to project value types. The adapter's
  Ghostty key encoder reads the current terminal modes and returns bytes plus
  modifier/Escape traits from the final physical Ghostty key. The worker uses
  those traits only after a nonempty encoding to apply live selection-clearing
  policy; sequence leaders remain byte-only and never acquire that policy when
  flushed.
- One typed IME value carries the commit and preedit transition through a
  single queued worker operation. Commits use the same encoded-key policy;
  preedit start/change/end clears independently when configured. The rendered
  preedit string remains a local UI overlay.
- Mouse events use Ghostty's mouse encoder when an application enables mouse
  tracking. Holding `Shift` retains local selection and scrollback behavior.
- Link hover requires exactly `Ctrl` on Linux. Explicit OSC 8 destinations take
  precedence over Ghostty's default regex-detected URL/path range. A matching
  result changes the pointer and underlines the visible matching cells; an
  already single-underlined cell becomes double-underlined while hovered. With
  application mouse capture, `Shift` first bypasses capture and is removed
  before modifier matching, so the equivalent gesture is `Ctrl+Shift`.
- `link-previews = true` shows the accepted raw destination for either link
  kind, `false` shows neither, and `osc8` shows only explicit OSC 8
  destinations. This presentation policy does not change link detection,
  underline, copy, or activation behavior.
- Focus changes are encoded only when the terminal requests focus reporting.
- Paste classification, retained unsafe text, and encoding stay on
  `SessionWorker`. One adapter operation snapshots Ghostty's current
  bracketed-paste mode for both the exact safety decision and encoding.
  Rejected text receives a nonzero worker-local request ID. The GUI retains an
  immutable display/grouping copy plus queued `(pane, request ID)` targets,
  but never uses that copy as paste input. A separate workspace dialog token
  prevents duplicate or stale accept/reject callbacks from consuming the next
  request. Confirmation consumes the original worker request once, snapshots
  current terminal mode again, and bypasses protection without rereading
  `QClipboard`. Only an accepted paste marks activity, returns to the active
  screen, and queues PTY bytes. Pane removal, session exit, and cancellation
  invalidate pending targets without writing.
- Selection is stored in Ghostty's terminal model and formatted by Ghostty for
  clipboard copy. Formatting, destination intent, and optional explicit-copy
  clearing are one `SessionWorker` operation; the resulting immutable text and
  typed intent cross to a queued GUI receiver, so only the GUI thread
  reads or writes `QClipboard`. Linux copy-on-select commits on left-button
  release and select-all, with primary-selection fallback resolved from Qt's
  GUI-thread capability. Libghostty's tracked selection-gesture state keeps
  the drag anchor stable across output, scrolling, resize, and automatic
  selection clearing. Raw binding actions, paste, mouse/focus reports, and
  replayed sequence leaders bypass clear-on-typing. OSC-driven clipboard writes
  from terminal applications are denied by the host callback.
- Typed viewport requests cover top, bottom, signed row deltas, absolute rows,
  and the current selection. Select-all and endpoint-adjustment operations run
  as single adapter calls on the session thread. Selection snapshots contain
  untracked Ghostty grid references, so fetch, adjustment, coordinate
  conversion, installation, and endpoint autoscroll are deliberately one
  transaction with no queued boundary between them.

The render update exposes only whether a cell has an OSC 8 hyperlink; regex
text and matcher state never cross to the GUI. Lookup remains beside the
terminal on the session thread: a hover request sends the viewport-relative
cell and retained content revision. That revision is an initial-coordinate
handshake only. The worker collapses queued pointer queries to the newest
coordinate. It first probes the requested cell for an explicit OSC 8 target.
When no explicit link owns the cell and `link-url` is enabled, the adapter
snapshots the complete
semantic logical line containing the cell and maps every emitted UTF-8 byte
back to its Ghostty screen cell. Soft wraps are removed, semantic prompt-state
boundaries are retained, spacer cells do not duplicate wide graphemes, and
combining sequences keep their exact byte-to-cell mapping.

`libghostty-vt` deliberately compiles without Oniguruma, so the default matcher
uses a separate project-owned Zig/C ABI. Its expression is imported directly
from the pinned `src/config/url.zig`, its Oniguruma library comes from the same
pinned Ghostty package, and each search uses Ghostty's 100,000-step retry
limit. The narrow C++ owner returns only half-open UTF-8 byte ranges. This keeps
regex semantics and the upstream corpus exact without adding a private matcher
handle or an Oniguruma dependency to libghostty-vt.

An accepted OSC 8 target owns one tracked grid reference and caches its original
destination bytes and primary/alternate-screen owner. An accepted regex target
owns tracked start, end, and queried-cell references plus the matched bytes and
the formatter text covered by its inclusive endpoint cells. Resolving either
target converts its anchors back to
current viewport cells, so scrolling, primary-screen reflow, and wide-cell
decoration remain stable. Regex resolution re-extracts the covered range and
requires byte equality, while a relevant logical-line update reruns matcher
precedence; replacement fails closed rather than transferring a hover or press
to new text. An inactive screen or wholly off-viewport target is
temporarily hidden, while reset, scrollback pruning, missing anchors, or changed
covered text permanently invalidate it.

The worker refreshes sparse anchors only when a full/scrollbar/geometry update
or relevant dirty row can affect their visible range. Unrelated output can
therefore advance the pane-wide content revision without clearing the pointer,
re-running the matcher, or rescanning the viewport. Cancellation crosses the
queued boundary and frees every anchor on the session thread before terminal
destruction. Disabling `link-url` live cancels a regex lease and recomputes a
stationary hover; explicit OSC 8 interaction is unaffected.

The worker does not retain a second full `TerminalFrame` for OSC 8 lookup. It
applies each render delta to a sparse, per-row index of linked columns. Dirty
rows replace only their own index entries, and the row-major point list needed
by URI grouping is materialized lazily and reused until indexed positions
change or a full frame replaces the viewport. Metadata-only revisions therefore
update the coordinate handshake without allocating or scanning the viewport.

Public `libghostty-vt` exposes the destination URI but not the OSC 8 hyperlink
ID. OSC 8 hover grouping therefore compares the URI of every visible candidate:
two separate IDs with the same URI can be underlined together. Regex grouping
is the tracked matched range itself. Activation remains cell-specific. The raw
render bit lets an OSC 8 left press arm while its hover lookup is still pending.
A regex press arms only over the pane's already-resolved regex hover mask. Once
armed, the press creates a separate OSC or text-range lease. Drag, focus loss,
or cancellation frees that lane independently.
A release below Qt's drag threshold activates only when the tracked target still
resolves to the release coordinate with its original URI or covered text;
unrelated output between press and release does not cancel the gesture.

The value used by `copy_url_to_clipboard` remains byte-exact through the worker
lookup and is put directly in the clipboard's `text/plain` MIME payload, never
round-tripped through `QString` or `QUrl`. For opening, the GUI converts
absolute paths with `QUrl::fromLocalFile`. A regex-matched relative path is
resolved against the terminal's current working directory when the resulting
file exists. Other byte strings use `QUrl` strict encoded mode; malformed or
NUL-containing values are rejected before valid URLs are delegated to Qt's
desktop services.

The preview is a pane-local scene-graph overlay built from the exact accepted
destination bytes rather than the normalized opener `QUrl`. Its display source
is capped at 4,096 bytes, decodes malformed UTF-8 with visible replacement,
escapes C0/C1 controls and bidirectional/line-formatting controls, and is
middle-elided to the available pane width. Both dimensions are clipped to the
pane. It normally occupies the bottom-left; entering that original guard keeps
the logical hover lease and relocates the presentation to the bottom-right,
avoiding an input-owning child item or a query against the obscured terminal
row. The overlay consumes the URI and link kind already returned for the
tracked hover. Showing, hiding, relocating, resizing, or live-reloading its
policy performs no extra worker query or regex matcher scan while the pointer
remains over the terminal link. If policy or geometry removes an occupied
bottom-left guard, the pane resumes hit testing at the physical pointer and may
query that newly exposed terminal cell.

Terminal search is also owned by `SessionWorker`, but it does not call
Ghostty's application search thread. The pinned terminal module explicitly
defines `terminal.search.Thread` as `void` for a library artifact: the upstream
thread is currently available only to the Ghostty application because it
depends on `xev`. The public `libghostty-vt` C API exposes neither that thread
nor an equivalent search entry point. The adapter therefore provides a narrow,
value-only foundation: the extent of the active screen and scrollback, one
physical-row snapshot at a time, UTF-8-byte-to-cell ownership, visible-cell
resolution for a result, and result scrolling. Ghostty handles and untracked
grid references never leave the adapter call.

Those public screen-coordinate reads differ from Ghostty's internal search
window in two important ways. Reading a grid reference in compressed history
temporarily restores its page instead of decoding it into temporary owned
storage, so the worker interleaves libghostty's bounded compression traversal
during a scan and starts a final pass when a scan completes, is canceled, or is
replaced. The public API also exposes a flat row sequence without internal page
boundaries or `PageFormatter` trailing state. The frontend can reproduce the
ordinary literal, soft-wrap, and hard-line behavior, but it cannot exactly
reproduce every page-boundary delimiter or the pinned formatter's blank-cell
point-map quirks.

The worker scans those snapshots from newest to oldest in bounded row/time
chunks and yields to its Qt event loop between chunks. It coalesces progressive
publication to roughly 30 Hz so deep history does not create one queued GUI
event per scan chunk. A generation token cancels superseded needles without
publishing stale results, while updates carry rows scanned, rows total, the
match count, and only the currently visible candidate/selected cell masks.
Literal UTF-8 byte matching is
ASCII-case-insensitive, permits overlapping results, removes soft-wrap
boundaries, and retains hard line boundaries as newline bytes. Next selects the
newest result first and then walks older results; previous starts at the oldest
and walks newer results. Both wrap, and selection scrolls only when the result
is wholly outside the viewport. Unlike Ghostty's dedicated viewport search, the
frontend derives candidate highlights from results already found by the global
bottom-up scan. Scrolling into older history can therefore show candidates late,
when that scan reaches the newly visible rows.

Terminal-data mutation increments a search-specific revision and restarts the
active query so no value result is applied to changed grid content. A viewport
scroll only recomputes visible decorations and does not restart the scan. This
is intentionally an incremental compatibility foundation: it can redo work
during continuous output, searches only the currently active primary or
alternate screen, and does not preserve independent result sets while the
other screen is inactive. Together with the flat-row formatter and viewport-lag
differences above, those gaps remain until a stable upstream search API is
available or the public adapter can own tracked results across screen switches.
The byte matcher checks its cooperative time budget within a row, but the
public adapter currently prepares one complete physical-row snapshot first;
an adversarial maximum-width row or unusually large grapheme can therefore
exceed the ordinary slice duration before matching yields.

`TerminalWorkspace` creates one pane-local QML search overlay through a shared
component factory. `start_search` shows it without allocating scan work until a
nonempty needle exists; reopening retains the text and requests select-all.
`search:<text>` replaces or stops the engine needle without changing overlay
visibility, `search_selection` copies the current untrimmed selection into the
overlay, `navigate_search:next|previous` changes the selected result, and
`end_search` stops the engine and hides the overlay. The entry debounces edits,
shows progressive selected/total status, and maps Enter, Shift+Enter, and
Escape to next, previous, and end. It is anchored at the top-right and is not
draggable yet.

Resize starts in `TerminalPane`: font metrics and item geometry determine rows,
columns, cell pixels, and surface pixels. The worker resizes both Ghostty's
terminal and the kernel PTY with `TIOCSWINSZ`.

After output becomes idle, the worker runs libghostty's bounded incremental
scrollback compression until the current pass completes. Compression and all
other terminal access remain serialized on the pane's worker thread.

## Session and environment

The first pane receives the parsed command-line options. Without a command,
the worker starts executable `$SHELL` or `/bin/sh`. Later surfaces start the
default shell and derive launch-only values from the newest workspace options.
A new tab retains the exact action-target pane; an empty-context QML request
uses the current tab's recorded active pane. When
`tab-inherit-working-directory` is true, that source's latest nonempty reported
directory takes precedence; when false, or when the source has no
terminal-owned directory, the workspace `working-directory` is retained.
`window-inherit-font-size` independently copies only the source's actual point
size, including manual zoom, or retains configured `font-size` when false. The
font family always comes from the newest configuration. The tab child starts
unadjusted, so a later font-size reload replaces its inherited visible size.

A split starts the default shell using the workspace's effective directory and
policy: when
`split-inherit-working-directory` is true, the explicit source pane's latest
nonempty reported directory takes precedence; when false, or when the source
has no terminal-owned directory, the workspace directory is retained. In both
modes the source contributes its current font and effective font size. The
inherited size is the child's reset target until a later config reload replaces
it; the child starts unadjusted, so that reload also updates its visible size.

The libghostty-vt terminal callback exposes raw OSC 7 data because Ghostty's
application stream handler normally performs the surrounding policy. The Qt
adapter mirrors that policy at its boundary: a direct C++23 port of the pinned
`std.Uri` branch structure accepts only `file` and `kitty-shell-cwd` URIs naming
`localhost` or the dynamically queried current machine. It preserves Zig's
userinfo, bracketed-host, port, percent-decoding, and path boundaries; Linux
MAC-shaped hostnames then use Ghostty's six-octet standard-parse/fallback
repairs. File paths are decoded while kitty's raw-path form is preserved. A
remote, parser-rejected, missing-host, or unsupported URI therefore cannot
become a child launch directory. Validation occurs for every callback, so a
later invalid URI in one coalesced VT batch cannot erase an earlier accepted
local update.
`inherit` performs no `chdir` and leaves the host `PWD` untouched. A concrete
request is copied verbatim to `PWD`. Immediately before spawning, a missing
directory drops the child `chdir`; any existing path is attempted, but a child
`chdir` failure is non-fatal. The command therefore starts in the application
process directory while the requested logical `PWD` and GUI directory remain
stale until a later accepted OSC 7 update, matching pinned Ghostty. Concrete
strings are not lexically normalized, because removing `..` across a symlink
can select a different directory.
Relative explicit programs and relative `PATH` entries are likewise resolved
from the effective child directory without lexical normalization. Matching the
pinned Zig launcher, empty `PATH` components are skipped, an unset `PATH` uses
`/usr/local/bin:/bin/:/usr/bin`, and `ENOENT`/`ENOTDIR` candidates fall through
to the next entry after the child changes directory.

The child inherits the host environment with these terminal-specific values:

```text
TERM=xterm-ghostty
TERMINFO=<resolved private database>
COLORTERM=truecolor
TERM_PROGRAM=ghostty-qt
TERM_PROGRAM_VERSION=<project version>
PWD=<exact concrete request, even on cwd fallback; inherited unchanged for inherit mode>
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

The helper process necessarily has Ghostty action arguments, so the pinned
parser classifies it as a probable CLI launch. An otherwise unset
`working-directory` therefore finalizes to `inherit`; the GTK desktop-launch
heuristic that would choose `home` cannot be reconstructed from the current
text protocol. Explicit `inherit`, `home`, tilde, and concrete path values are
still preserved, and the parity ledger keeps this setting and the dependent
tab/split fallbacks partial.

The current typed compatibility slice contains `working-directory`,
`split-inherit-working-directory`, `tab-inherit-working-directory`,
`window-inherit-font-size`, `font-family`, `font-size`, the
appearance keys listed below, the frontend-only `unfocused-split-opacity`,
`unfocused-split-fill`, and `split-divider-color`,
`scrollback-limit`, `confirm-close-surface`,
`link-url`, `link-previews`, `config-file`, and a versioned dump of the
finalized keybinding sets.
Appearance crosses threads as a
value-only `TerminalAppearance`: terminal foreground/background, all 256
palette defaults, selection and candidate/selected search colors, cursor
color/style/blink/opacity/text, bold-color, and faint-opacity. Fixed colors and
Ghostty's cell-foreground and cell-background aliases remain distinct until
the renderer has the target cell. Only the first configured font family is
rendered. Explicit working-directory, font, and scrollback CLI options retain
precedence.

Live reload updates font and appearance on existing panes without overriding a
pane's manual font zoom. Directory and font inheritance booleans are
workspace-owned creation policy: they affect future tabs/splits without moving
an existing process or marking an inherited child as manually zoomed. Palette
and fixed cursor defaults are updated through
`libghostty-vt`, which preserves terminal-originated OSC 4/OSC 12 overrides;
OSC 104/OSC 112 reset to the newest configured defaults. Likewise, an active
DECSCUSR cursor style survives a config reload and its reset selects the newest
configured style. Selection, search, cursor aliases/opacity/text, bold-color,
and faint-opacity are frontend render policy and therefore update without
mutating terminal-originated state. Close confirmation policy and the built-in
regex link matcher also update live; toggling `link-url` never disables OSC 8.
The nullable divider color likewise reloads entirely on the UI thread: a fixed
RGB value paints the exact reserved gaps, while an empty canonical value
removes those nodes without relayout, focus changes, or terminal-state work.
Unfocused split appearance also reloads entirely on the UI/render side. A pane
is dimmed only while its tab root is structurally split, its actual terminal
and window focus is absent, and its own search UI is closed. The nullable fill
resolves at presentation time to the configured background rather than the
terminal frame's OSC 11 state; the retained rectangle uses the complement of
Ghostty's finalized pane opacity. Split membership changes only when a split
is created or collapsed, remaining true through zoom and staying out of the
divider-drag layout path.
The three-state link-preview policy reloads entirely in each pane's frontend and
preserves an accepted hover over the terminal link. Removing an occupied
preview guard instead resumes physical hit testing, as pointer ownership has
changed.

Working-directory and split-inheritance reloads are workspace-owned launch
policy. They do not replace a running pane's directory, rebuild its rendering,
or cross the session thread boundary; they are consulted only when a later tab
or split is constructed. Building split options from the newest workspace base
also prevents a nested split from retaining an older configured fallback.

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
`GhosttyActionCatalog`. Catalog parsing routes workspace actions through the
same typed dispatcher used by QML controls, while viewport, font-size,
selection, search, terminal-control, and named-key-table actions become owned
typed pane requests. Pane-local copy, paste, and reload actions still use their
terminal operations directly.
Page actions use the full terminal height; fractional pages multiply in f32
and truncate toward zero, while line and absolute-row parameters retain their
pinned i16 and usize bounds. Non-finite or unsafe fractional values are
rejected instead of reproducing the pinned frontend's float-to-integer crash.
The pane tracks the latest queued resize height so a page action cannot regress
to a stale rendered frame while resize is in flight. The controller likewise
tracks queued select-all intent for action-chain performability; the worker
reports completion even on a blank terminal, reconciling that intent with the
authoritative selection state without exposing speculative QML state.
This closes deterministic action-chain ordering; a separate key event during
the reconciliation window can still observe pending or cached state. Moving
the performability decision into worker-side input dispatch is the remaining
boundary for exact selection-dependent timing.

Terminal-control actions follow the same catalog-to-pane-to-controller route
but mutate the session only on `SessionWorker`. The structured Ghostty helper
formats every byte-string action with `std.zig.stringEscape`; the worker first
inverts that canonical transport encoding. `csi` and `esc` then prepend their
introducer and enqueue the complete byte sequence once, while `text` applies
Ghostty's separate config-string escape parser before writing. This two-stage
decode preserves Unicode, control bytes, and embedded NUL without confusing
transport escaping with text-action semantics. A malformed text literal is
still consumed but writes nothing. `reset` calls the public libghostty full
reset, invalidates the retained frame, and synchronizes mouse, selection,
title, and working-directory caches before publishing a complete replacement.
The controller also performs a pure decode preview for newline-bearing actions
so an immediate close request cannot outrun the worker's active-process signal;
the actual terminal mutation remains worker-owned.

`GhosttyApplicationKeybindings` performs root app-scoped leaves before the
focused pane lookup, matching Ghostty's app/surface split while leaving leaders
and mixed-scope chains to the pane. A pane that matches `all` or `global`
forwards the chain to that process controller. It executes app actions once and
surface actions over a stable pane snapshot, action-major across the chain;
`unconsumed` and `performable` do not alter broad-binding consumption. Split
container actions resolve from each tab's current active pane during that
fanout, matching the pinned GTK split-tree action boundary. Automatic split
direction is resolved from each originating surface before that placement
redirect. New-tab actions instead retain every pane in the stable fanout
snapshot as their creation source, so activating one new tab cannot redirect
the next action's inherited directory or font size. A fullscreen surface
action is coalesced once per workspace during
broad fanout because every pane maps to the same synchronously toggled Qt host
window; separate workspaces still receive independent transitions.

On Linux, eligible root `global` bindings are registered asynchronously through
the XDG Global Shortcuts portal using Qt D-Bus. Request response subscriptions
are installed before calls, and config generations reject stale create/bind
callbacks. Reload closes the previous session before registration. Matching the
pinned GTK frontend, only direct root bindings with one action are portal
eligible; sequences, catch-all triggers, table entries, and action chains are
diagnosed and skipped. Portal failure is nonfatal and never falls back to a
focus-only Qt shortcut.

## Build integration

The source tree reads its authoritative Ghostty commit from
`GHOSTTY_REVISION`. CMake validates the submodule revision when the checkout
has Git metadata and rejects a mismatch unless upgrade work explicitly enables
`GHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY`.

Ghostty's CMake wrapper invokes Zig and exports `ghostty-vt-static`. The project
checks `zig version` at configure time and accepts only `0.15.2`, matching the
pinned Ghostty source. One internal `ghostty-qt-vt-adapter` target privately
links that engine and contains its C API; the application and focused tests
reuse the compiled adapter. The executable separately links Linux `libutil`.

Ghostty's exported VT module intentionally disables Oniguruma. CMake therefore
builds `zig/link_matcher` as a second, narrow static C ABI, importing the pinned
default expression and vendored engine without modifying the Ghostty submodule.
Revision/optimization-scoped outputs live under the ignored
`.cache/ghostty-link-matcher` directory; an install lock lets CMake presets
reuse them safely. The main executable sees only the Qt-valued C++ wrapper.

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

`docs/ghostty-parity.json` references the same authoritative revision file as
CMake, records the Linux/Wayland/Qt scope, and inventories upstream
configuration keys, keybinding actions, and CLI actions with explicit status
and scope labels. The target is to preserve portable and Linux Ghostty
configuration names and semantics while mapping meaningful GTK frontend
behavior onto Qt. GTK-only presentation/debugging internals, X11, macOS, iOS,
and FreeBSD behavior are not part of this frontend's parity target.

`scripts/check-ghostty-parity.py` re-extracts those inventories from the pinned
source and rejects revision-file, schema, ordering, or inventory drift. This
keeps an upstream snapshot update from silently adding untracked parity work.
The contract remains conservative: only the typed configuration slice,
including the four search colors, `link-url`, and `link-previews`, is marked
partial or supported. Search actions remain partial because the public library
artifact cannot expose Ghostty's `xev`-dependent search thread, while custom
`link` rules and other upstream keys stay explicitly planned. In particular,
the pinned Ghostty `RepeatableLink.parseCLI` still returns
`error.NotImplemented`, so this frontend does not invent a parallel syntax for
user-defined expressions and actions.

## Test boundaries

The default CTest suite has focused layers for each ownership boundary:

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
  focus, and key input using terminal modes. It also verifies tagged viewport
  scrolling, selection-target alignment, select-all, and endpoint adjustment
  with exact autoscroll and configurable selection trimming, plus
  primary/alternate-screen reset, mode clearing,
  selection invalidation, history removal, forced full-frame publication, and
  long OSC 8 URI lookup across active, scrollback, alternate-screen, and reset
  state. Tracked URI anchors are also exercised across unrelated output,
  reflow, viewport hiding/restoration, screen switches, replacement, reset,
  and scrollback pruning. Logical-line snapshots additionally cover exact UTF-8
  mapping across combining graphemes, wide cells, soft wraps, semantic prompt
  boundaries, range reflow, text replacement, and scrollback pruning. Search
  snapshots cover trimmed physical rows, byte-to-cell ownership, hard-newline
  versus soft-wrap behavior, visible range mapping, and viewport alignment.
- `ghostty-link-matcher` verifies the C++/C ABI, successive UTF-8 byte ranges,
  scheme/path heuristics, punctuation, IPv6, and invalid-coordinate handling.
- `ghostty-link-matcher-upstream-corpus` runs the complete pinned Ghostty
  `src/config/url.zig` corpus against the vendored Oniguruma implementation.
- `session-worker` starts real PTY children and verifies DA replies, bracketed
  paste fence bytes, staged sequence ordering and stage-time VT modes, final
  output draining, byte-exact terminal-control action writes, reset cache
  synchronization, process exit, explicit-program activity, and an interactive
  shell's idle/job/idle foreground transitions. It also verifies coalesced OSC
  8 hover queries, stale-coordinate retry signaling, tracked targets across
  viewport hiding/restoration, and independent hover and activation leases. It
  also verifies regex lookup across UTF-8 graphemes and soft wraps, OSC 8
  precedence, range reflow, viewport hiding/restoration, stable unrelated
  output, and activation invalidation after covered-text replacement. Search
  tests exercise bounded progressive scans, superseding generations, content
  mutation restarts, overlapping ASCII-insensitive matches, navigation, and
  selection-derived needles.
- `terminal-workspace` verifies that active programs request confirmation,
  idle shells follow `true` versus `always`, pending quit resolves on process
  exit, approval is emitted once, and workspace navigation/layout actions
  preserve stable tab and pane identity. PTY-backed new-tab coverage verifies
  explicit binding sources, empty-context active leaves, broad-fanout source
  stability, local OSC 7/reset fallback, manual font zoom, and future-creation
  policy reloads. It also sends real pointer gestures
  through nested divider gaps to verify exact-split targeting, T-junctions,
  focus preservation, endpoint clamping, cancellation, zoom/tab/scene
  lifecycle, ratio persistence, terminal-cell hit regions, and nullable live
  divider recoloring. A focused second CTest run repeats the nested drag and
  color capture at a scale factor of two to keep geometry in logical
  coordinates while checking physical pixels.
- `workspace-foundation` verifies stable tab identity after row removal and
  movement, tab model role updates, and typed action context dispatch.
- `ghostty-action-catalog` verifies the supported subset of pinned Ghostty
  action-string parsing—including the five search actions and exact navigation
  grammar—and deterministic malformed/unsupported results.
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
  `clear`/`unbind` resolution, including canonical byte-string action export
  and nullable X11 divider-color canonicalization.
- `ghostty-config-helper-smoke` runs `+validate-config` through the helper and
  exact pinned Ghostty parser built for the application.
- `terminal-pane-render` renders frames offscreen, verifies the initial
  placeholder is replaced plus selection/cursor/text appearance, and exercises
  sequence consume/replay, performability, viewport/selection action routing,
  release suppression, reload cancellation, and tracked OSC 8 hover, copy, and
  release-only activation through a real PTY-backed pane, including live
  output, viewport hiding/restoration, resize-safe masks, and mouse-capture
  modifier transitions. The same path covers live `link-url` enable/disable,
  byte-exact regex copy, relative-path opening, OSC 8 independence, all three
  link-preview policies, live frontend-only reload, no-query relocation, and
  bounded/escaped display of arbitrary destination bytes. Search rendering
  covers candidate/selected/terminal-selection precedence, resize-safe masks,
  live colors, overlay state, and the distinction between UI and engine-only
  actions.
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

Clipboard and selection-lifecycle tests cover trim policy, copy destinations
and primary fallback, explicit copy-and-clear ordering, automatic selection
commits, select-all, live reload, middle-click source/ignore policy,
clear-on-typing key traits, sequence replay exclusions, and IME/preedit
transitions. Paste-safety tests cover Ghostty's exact bracketed and
non-bracketed policy, live options, control-byte encoding, confirmation-time
mode changes, accepted-only activity and viewport changes, all GUI paste entry
points, immutable worker IDs, multi-pane dialog correlation, queued payloads,
stale responses, session/pane teardown, and preview bounds.
Typed-action tests cover tab and split state transitions; the offscreen tests
validate QML startup, close-dialog shutdown, and scene-graph frame replacement
in a headless environment, but they do not validate the hardware RHI path.
`GHOSTTY_QT_ALLOW_NON_WAYLAND=1` is a test escape hatch rather than a
supported runtime configuration; GPU output must also be checked interactively
in a real Wayland session.

## Deliberate renderer-v1 limits

- Dirty-row value updates keep the thread boundary small for ordinary output,
  and persistent row text nodes keep per-cell `QTextLayout` work local to those
  rows. Transient solid geometry still scans the visible grid and rebuilds by
  painter layer; larger compatible text runs and retained geometry remain
  future optimizations.
- Text uses Qt's GPU distance-field glyph atlas on hardware RHI backends, but
  there is no ligature shaping across terminal cells, color-emoji pipeline, or
  Kitty graphics/inline-image renderer.
- Public `libghostty-vt` cannot preserve configured cursor-blink tri-state
  precedence over DEC mode 12, and the text config dump cannot expose the
  post-generation palette mask. Those cases remain explicitly partial/planned
  in the parity ledger.
- Configuration beyond the documented typed slice, unsupported keybinding
  actions, user-defined `link` rules, multi-window operation, saved sessions,
  and production packaging remain future work. OSC 8, the default `link-url`
  matcher, link previews, and the incremental search foundation are
  implemented. Search remains partial because the library artifact omits the
  upstream `xev`-dependent thread, mutation restarts its scan, inactive-screen
  results are not retained independently, and the overlay is not draggable.
  Custom `link` parsing remains unavailable in the pinned parser, and OSC
  grouping stays URI-based until the public C API exposes hyperlink identity.
