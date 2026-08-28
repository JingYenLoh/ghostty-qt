# Architecture

ghostty-qt is a Linux/Wayland Qt frontend around Ghostty's terminal engine.
This document describes stable ownership and data-flow boundaries. User-visible
coverage belongs in [Project status](status.md); benchmark procedures and
optimization candidates belong in [Performance](performance.md).

## Scope and stack

| Layer | Technology | Responsibility |
| --- | --- | --- |
| Application and chrome | Qt Quick, QML, Quick Controls 2 | Windows, toolbar, tabs, menus, dialogs, and overlays |
| Workspace and terminal item | C++23, Qt Quick | Split trees, pane identity, input routing, and scene-graph integration |
| Session | Qt Core on one `QThread` per started pane | PTY I/O, child lifecycle, terminal mutation, and frame publication |
| Terminal adapter | C++23 value boundary | Owns all `libghostty-vt` handles and translates to project types |
| Terminal engine | Zig-built static `libghostty-vt` | VT parsing, screen state, selection, and input encoding |
| Configuration | Qt services plus a private helper process | Shared Ghostty parsing and strict Qt-owned configuration |
| Linux integration | Wayland, D-Bus, portals, systemd protocols | Activation, shortcuts, notifications, quick terminal, and cgroups |
| Build | CMake/Ninja plus Zig 0.16.0 | Qt targets, pinned Ghostty, resources, and tests |

C++ is the host language because Qt and QML expose native C++ ownership and
rendering APIs. The terminal runtime remains Zig-owned behind the public
`libghostty-vt` API; no Zig or Ghostty handle crosses into QML or the scene
graph.

The supported platform is deliberately narrow: Linux and Wayland only. X11,
other operating systems, and GTK/libadwaita implementation details are not
abstracted into the application.

## Runtime ownership

The principal ownership graph is:

```text
ApplicationController                         GUI thread
  -> zero or more application windows
     -> TerminalWorkspace                     tabs and recursive splits
        -> TerminalPane                       QQuickItem and pane UI state
           -> TerminalController              GUI-side session proxy
              -> SessionWorker                dedicated session thread
                 -> GhosttyVtAdapter          all libghostty handles
                 -> PTY master <-> child process group

GhosttyConfigService                          GUI-side coordinator
  -> ghostty-qt-config-helper                 short-lived child process

FrontendConfigService                         GUI-side coordinator
  -> strict file loader                       worker thread

GhosttyApplicationKeybindings                 process lifetime
  -> immutable keybinding program
  -> per-pane matcher state
  -> XDG Global Shortcuts portal
```

QML owns presentation, not terminal state. `TerminalWorkspace` and
`TerminalPane` are C++ `QQuickItem` types exposed to QML, which lets structural
state remain typed and testable while controls and dialogs stay declarative.

The application uses four execution contexts:

- the GUI thread owns QObjects, workspaces, input events, and application
  policy;
- the Qt render thread owns scene-graph nodes and GPU resources;
- each started pane has one session thread for ordered PTY and terminal work;
- configuration helpers and selected background jobs run outside those hot
  paths.

Communication across these boundaries uses queued requests and owning value
types. Borrowed libghostty data is copied before a callback returns. QSG nodes
and graphics resources never leave the render thread.

## Identity, ordering, and lifetime

Tabs, panes, windows, splits, and correlated dialogs use monotonically assigned
stable IDs. Deferred work resolves an ID at execution time instead of retaining
container indices or raw pointers across event-loop turns. This is essential
because Qt signals can synchronously close a pane, reorder tabs, or destroy a
window.

Configuration, search, hyperlink, inspector, and dialog requests carry
generations or request IDs. Consumers reject stale results. Broad actions
snapshot stable targets, revalidate each target before use, and preserve the
defined action order even when a target disappears.

Closing is a transaction rather than an immediate object deletion:

1. assess live process/read-only policy;
2. correlate any required confirmation with its stable target;
3. commit the topology change or application shutdown;
4. stop accepting new structural work for a committed workspace;
5. drain and join its session workers before final destruction.

This design prevents late worker publications, nested signal handlers, or a
reused model row from redirecting an operation.

## Launch and session boundary

`LaunchOptions` contains process and frontend policy. Before a pane starts, its
session projection is combined with an optional one-shot initial geometry to
build `TerminalSessionLaunchOptions`. The result contains only values needed by
the session: command, environment, working directory, terminal identity,
history limits, cgroup policy, appearance, and geometry.

Launch-only state is snapshotted when a pane is created. Existing sessions do
not retroactively change their command, environment, PTY identity, cgroup, or
scrollback allocation. Live appearance and behavior are projected separately
as `TerminalSessionRuntimeOptions` and queued to the worker. Identical updates
are ignored.

The initial-session coordinator serializes one-shot command and hold options.
The lease is committed once terminal initialization succeeds, so concurrent or
deferred panes cannot consume the same initial command.

### Child creation

The worker prepares a child environment from inherited values, generated
terminfo paths, Ghostty configuration, and explicit `env` overrides. It
finalizes shell integration through the pinned helper, then launches the child
with `forkpty`.

The child side applies the requested working directory, process group,
environment, and command. Linux cgroup placement can gate execution until the
parent has created and verified a transient user scope. Its configured
soft-failure policy releases the child when systemd is unavailable; hard
failure terminates it before user code runs.

After `forkpty`, the child executes only async-signal-safe setup and `exec`
operations. Qt, D-Bus, allocation-heavy preparation, and cgroup coordination
remain parent-side.

The PTY master is nonblocking and owned only by `SessionWorker`.

### PTY reads

A `QSocketNotifier` drives output reads. One activation:

1. reads into a 64 KiB buffer;
2. submits each nonempty chunk to `GhosttyVtAdapter`;
3. applies ordered deferred effects and mode changes;
4. schedules a coalesced frame publication;
5. yields after a bounded amount of work so control requests can run.

Ordinary activations process at most 1 MiB and continue on the event loop when
more data remains. Final shutdown uses a larger bounded drain to retain output
written immediately before child exit. The frame timer coalesces bursts over
8 ms instead of publishing one update per read.

### PTY writes

Keyboard, mouse, focus, paste, and terminal-generated replies are encoded by
libghostty on the worker thread. Terminal reply callbacks pass a synchronous
borrowed byte view; with no queued data, the worker writes that view directly
to the nonblocking master. A `PtyWriteBuffer` retains only an unwritten suffix
after a short write or `EAGAIN`; its consumed offset avoids moving that suffix
after every later write. While backpressured, new writes append in FIFO order
and wait for the write notifier instead of retrying the full descriptor from
each callback.

User input and protocol replies are distinct. Read-only mode suppresses
user-originated writes but not replies required by the active terminal
protocol.

### Exit and close policy

The worker tracks child PID, process group, foreground work, semantic prompt
state where available, and configured hold behavior. Child exit triggers a
final PTY drain before the terminal is marked complete. Close prompts are based
on process/read-only policy rather than the presence of a worker thread alone.

## Ghostty adapter

`GhosttyVtAdapter` is the only owner of terminal, render-state, selection, and
tracked-range handles. Its public header exposes project and Qt value types,
not Ghostty headers.

The adapter provides:

- terminal creation, reset, resize, viewport, and history operations;
- VT input and deferred effects;
- key, mouse, focus, and paste encoding;
- selection and public-grid snapshots;
- dirty-row render updates and terminal metadata;
- ordinary Kitty placement snapshots;
- tracked ranges for links and other mutation-sensitive operations.

Callbacks from libghostty may borrow strings, byte slices, cells, or decoded
image storage. The adapter materializes required data while the callback or
tracked handle is valid. The UI never inspects PTY output or runs a second VT
parser to infer private terminal state.

Features that cannot be represented through the public API are documented in
[Features requiring upstream Ghostty APIs](../REQUIRES_UPSTREAM.md).

## Output and frame transport

The output path has six stages:

1. the PTY notifier supplies bytes to libghostty;
2. libghostty mutates terminal state and queues effects;
3. `SessionWorker` requests a render update at the coalesced frame boundary;
4. the adapter copies full or dirty-row state into an owning `TerminalUpdate`;
5. a queued signal crosses to `TerminalController` and `TerminalPane`;
6. the pane installs dirty row payloads into its row-sharded retained
   `TerminalFrame` and schedules a scene-graph update.

Resize, viewport changes, and structural terminal changes may publish a full
frame. Ordinary output publishes only dirty rows. Qt containers are implicitly
shared across the queued boundary. The pane's outer row table detaches when a
render snapshot is holding it, then dirty row payload handles are installed;
clean rows and the previous render snapshot keep sharing their payloads. This
merge copies no `TerminalCell` objects.

Terminal effects such as title, working directory, bell, progress, clipboard,
and desktop notification, plus worker completions such as file actions, travel
beside frame updates as typed values. They are not recovered by parsing
rendered cells.

## Renderer

`TerminalPane` presents the retained frame through Qt's public scene-graph API.
The renderer is hybrid:

- terminal backgrounds, decorations, cursors, and padding use retained
  vertex-colored geometry on OpenGL/Vulkan;
- fixed-pitch printable ASCII can use a terminal-owned glyph batch and compact
  shared atlas;
- `QTextLayout` and `QSGTextNode` remain the general shaping and software
  fallback;
- Kitty images use packed straight-alpha RGBA assets, retained textures, and
  retained placement nodes;
- background images and colors are separate retained backdrop resources;
- frontend overlays remain outside terminal cell geometry.

Text is shaped by Qt. A row enters the glyph fast path only when layout proves
that every printable glyph remains one cell wide and on-grid. Complex scripts,
ligatures, fallback faces, wide cells, and other unsafe runs use the complete
Qt text path.

### Damage and retention

Rows retain independent text and solid-presentation epochs. Dirty terminal
rows, search-mask differences, hyperlink changes, and block-cursor movement
invalidate only affected rows.

Global appearance changes are mapped to dependency bits recorded per row.
Palette, default color, selection/search style, bold/faint policy, contrast,
and explicit opacity changes skip rows that do not consume the changed input.
Affected rows reuse their scene-graph topology and buffer capacity whenever
geometry and render context remain compatible.

Font, device-pixel ratio, backend, render context, or structural grid changes
can require broader invalidation. Software rendering retains row plans but
uses Qt-compatible simple rectangle nodes rather than the RHI geometry path.

### Scene-graph ordering

Persistent groups preserve terminal painter order:

1. backdrop;
2. Kitty layers below cell backgrounds;
3. cell backgrounds;
4. Kitty layers below terminal text;
5. cursor background and before-text decorations;
6. glyphs;
7. after-text and cursor decorations;
8. Kitty layers above terminal text;
9. terminal overlays such as input-method preedit;
10. pane overlays; and
11. unfocused-pane dimming.

Selection, search, and link presentation participate in the appropriate cell
background or decoration groups. The final dimming layer is frontend-owned and
does not mutate terminal colors.

### Custom shaders

With no configured shader, the terminal renders directly and allocates no
effect layer. With shaders enabled, a private terminal render item becomes the
source for a retained multi-pass `QSGRenderNode`. Intermediate passes
ping-pong through at most two retained textures; compatible pipelines,
bindings, uniform storage, and targets survive ordinary frames.

Shader sources are compiled away from the GUI and render threads and cached by
content and Qt version. Only OpenGL and Vulkan RHI backends are supported.
Compilation or runtime failures publish configuration diagnostics and restore
unfiltered rendering.

Qt adds a stable `iPaneTransition` uniform to the generated ShaderToy contract.
Dedicated enter and exit sources compile as phase-gated stages after Ghostty's
persistent custom-shader chain: each stage copies its input exactly outside its
assigned lifecycle phase. Opt-in frontend durations drive finite clocks
independently of Ghostty's continuous animation policy. Close commits retire
pane IDs and begin worker shutdown immediately, while visible pane scene roots
remain attached through the exit clock and one bounded final-frame grace. Pane,
tab, and whole-window close all use the same path; unsupported or inactive
rendering fails open to immediate structural removal.

Future renderer experiments and their qualification gates are documented in
[Performance](performance.md).

## Input

Qt events are normalized on the GUI thread and sent to the worker as typed
requests. The worker rechecks terminal modes and uses libghostty to encode
bytes, keeping mode changes and later input in one ordered stream.

### Keyboard and input methods

Native Wayland keyboard metadata supplies physical key identity, active XKB
layout/group, and modifier state. The input path supports Kitty keyboard
reporting, composed text, Qt input methods, held-key state, and modifier release
before focus loss. Synthetic events without native metadata use a conservative
fallback.

Application and pane keybindings run before terminal encoding. A consumed
binding never reaches the PTY; unconsumed or replayed input returns to the same
ordered worker queue.

### Pointer and selection

`TerminalPane` maps logical Qt coordinates to physical terminal cells. Mouse
tracking modes, Shift capture policy, alternate-screen wheel conversion,
horizontal buttons 6/7, focus-follow behavior, local selection, multi-click
classification, edge autoscroll, link activation, context menus, and inspector
cell picking share one precedence path.

Pointer operations that depend on current terminal state are validated by the
worker. Stale asynchronous link or selection results carry revisions and
cannot activate content that has moved.

### Paste, clipboard, drops, and files

Text paste and drag-and-drop enter the same protected-paste path. Confirmation
is workspace-owned, correlated by stable pane ID, and applies before bytes are
encoded. Terminal-originated clipboard writes cross back to the GUI as bounded
owning MIME payloads and obey the configured allow/deny/ask and byte-limit
policies. The worker waits on a cancellation-safe synchronous reply bridge so
protocol status and Kitty session grants reflect the GUI commit. Only the GUI
thread accesses `QClipboard` and `QMimeData`.

Terminal file actions snapshot and format terminal-owned data on the worker,
then send the owning bytes to a bounded two-job persistence pool. A
worker-local ordered effect queue rejoins completion before publishing the
copy, paste, or desktop-open path and before releasing later user input or
file completions. PTY reads and protocol replies remain live while the
filesystem job runs. Teardown cancels publication without joining the job;
its still-owned temporary directory removes any unclaimed artifact.

## Search, links, and inspector

Search uses public terminal snapshots and runs cooperatively so large history
does not monopolize the session thread. Results carry content revisions and
are merged incrementally. A visible-row probe can provide early candidates
while the canonical history scan continues.

OSC 8 links use terminal metadata. Default URL/path detection uses a narrow
project-owned Zig/C matcher built from Ghostty's pinned expression and
Oniguruma dependency. Tracked ranges preserve logical cells across output,
scrolling, and reflow; activation is revalidated before opening.

The inspector is opt-in and pane-scoped. While open, it requests bounded public
terminal snapshots and retains a bounded event ring for input, keybinding,
session, and frame diagnostics. Closing it stops capture and clears copied
payloads. It deliberately does not replay PTY bytes or expose private Ghostty
parser state.

## Workspace and windows

Each tab owns a recursive binary split tree. Leaves are `TerminalPane`
instances; internal nodes retain orientation, ratio, and stable split identity.
Divider handles resolve IDs on every event, so collapsing a neighboring branch
cannot leave a dangling tree pointer.

The workspace owns:

- tab insertion, reordering, selection, titles, and attention;
- split creation, resizing, navigation, equalization, and zoom;
- active-pane focus and model publication;
- close, paste, and title confirmation queues;
- broad action target snapshots and barriers;
- pane overlays and tab-level status derived from active panes.

QML consumes a `QAbstractListModel` and typed properties. It does not mutate the
split tree directly.

`move_tab_to_new_window` creates and registers an empty workspace before
transferring the selected tab as one live ownership transaction. The tab's
recursive tree, panes, PTYs, overlays, and stable tab/pane/split/surface IDs are
retained; only QObject/scene ownership and workspace signal routing are
rebound. Process-wide surface and keybinding registries retarget those same
entries in place, preserving Ghostty's surface order for `all:` and `global:`
actions. The source refuses to transfer its sole tab.

Multiple windows are process-owned. The application registry pairs stable
window and pane IDs for navigation, command-palette rows, notifications, and
D-Bus activation. A hidden quick terminal remains a retained LayerShellQt
surface with separate presentation and autohide policy.

Qt's implicit last-window quit is disabled. `ApplicationController` owns
process lifetime, including configured delayed exit, explicit quit, aggregate
worker shutdown, and the valid zero-window resident service state.

## Keybindings

The pinned helper exports Ghostty's finalized root and named-table bindings.
`GhosttyApplicationKeybindings` compiles one immutable trie/action program per
configuration generation. Every pane has independent mutable matcher state for
pending sequences and active named tables while sharing the program storage.

Dispatch has three scopes:

1. application actions handled by the process owner;
2. workspace actions for tabs, splits, and windows;
3. pane/session actions that may reach the worker.

`all:` and `global:` bindings snapshot eligible surfaces and execute in stable
order. Asynchronous pane actions use ordered barriers so later chain entries
and replayed input cannot overtake search, clipboard, file, or confirmation
effects.

Eligible direct global bindings are registered through the XDG Global
Shortcuts portal. Reload derives the complete portal registry first; an
unchanged registry retains its active session, while a changed registry closes
and replaces the session before publishing its new state. Portal failure is
nonfatal and does not create a separate focus-only binding.

## Configuration

Configuration is split by ownership:

```text
$XDG_CONFIG_HOME/ghostty/config
$XDG_CONFIG_HOME/ghostty/config.ghostty
$XDG_CONFIG_HOME/ghostty-qt/config
```

The first two files provide the shared Ghostty base. The last is a mixed final
override: its Qt-owned keys are parsed by ghostty-qt while every other line is
applied by the pinned Ghostty parser after the shared recursive configuration.
GTK-prefixed settings are not aliases for Qt settings.

### Shared Ghostty parser

The complete Ghostty application parser is not part of `libghostty-vt`.
`ghostty-qt-config-helper` therefore links a private revision-matched
`ghostty-internal` library and exports a strict schema-v7 value projection.
The GUI process never parses human-oriented `+show-config` output.

The process boundary contains the private Zig application API and lets the main
binary retain a small C++/libghostty interface. It also provides exact pinned
implementations for supported `+` actions, font discovery, themes, SSH, and
shell-integration finalization. Successful shell preparations use a bounded,
single-flight process cache keyed by the request and the complete helper,
environment, private-library, and resource identity. Only the expected
production helper/runtime layout is eligible; other helpers, uncacheable
requests, and failures retain the same direct helper fallback semantics.

### Reload

Both services perform their initial load synchronously before application
presentation. `GhosttyConfigService` then runs watched and requested helper
transactions outside the GUI thread; `FrontendConfigService` does the same for
the Qt-owned subset of the mixed file. Each domain publishes immutable
generations, retains its last good snapshot after failure, and reports
source-labelled diagnostics. A failed generation arms a bounded exponential
retry, starting at five seconds and reaching at most five minutes while no
reload event arrives. File-watcher changes and explicit reloads reset that
backoff so a repaired configuration is attempted at normal debounce latency;
success also restores the initial retry interval.

The application rebuilds effective options from defaults, the shared Ghostty
base, the mixed override, and explicit CLI overrides. Live fields update
existing objects; construction and process fields become defaults for later
objects.
Reloading cannot change the running process's application identity or
single-instance role.

Exact files, precedence, and keys are documented in
[Configuration](configuration.md) and
[Frontend configuration](frontend-configuration.md).

## Desktop integration

Before Qt starts, raw command-line classification selects one of three paths:

- replace the process with the pinned helper for a supported delegated `+`
  action;
- run a minimal D-Bus client for `+new-tab`, `+new-window`, or
  `+toggle-quick-terminal`;
- construct the full Qt application.

The full application can own the standard `org.freedesktop.Application`
endpoint and compatible action interface. Single-instance launchers either
forward a typed activation or continue independently according to frontend
policy. Activation tokens are scoped to window presentation and removed from
the inherited terminal environment unless an explicit `env` override restores
them.

Every pane receives a nonzero process-wide surface ID before its child starts,
published as `GHOSTTY_SURFACE_ID`. The application registry maps those IDs to
live window/pane identities. A `+new-tab` `(tas)` action targets that registry,
then the focused workspace, and finally a new window; its command, working
directory, shell-integration, and title overrides are confined to the created
tab's first pane.

Installed D-Bus metadata can delegate cold activation to a systemd user unit.
Readiness and reload notification use `NOTIFY_SOCKET` directly; ghostty-qt does
not add a direct `libsystemd` dependency. `SIGUSR2` reaches Qt through a
nonblocking self-pipe and triggers both configuration domains.

KDE integration remains conditional:

- `qqc2-desktop-style` supplies controls matching the Plasma style when
  discoverable;
- KF6 WindowSystem supplies KWin blur when built in;
- LayerShellQt supplies the quick-terminal surface;
- the XDG Global Shortcuts portal supplies eligible global bindings.

Missing optional services degrade without preventing a terminal session.

## Build and upstream boundary

`GHOSTTY_REVISION` is the authoritative upstream pin. CMake verifies the
submodule checkout and builds:

- static `libghostty-vt`;
- the private configuration/helper runtime from a project-local source shadow;
- the narrow pinned link matcher;
- generated terminfo, themes, and shell-integration resources.

The official Ghostty submodule remains unmodified. Overlay sources are
versioned under `zig/`; they are applied to Ghostty only in generated
build-tree shadows. Changes that need a public Ghostty contract are recorded in
[`REQUIRES_UPSTREAM.md`](../REQUIRES_UPSTREAM.md).

Ghostty's source-tree Zig output is shared across CMake presets. Sequential
preset changes are detected and rebuilt, but concurrent preset builds in one
checkout are unsupported.

An intentional upstream upgrade updates the revision file, submodule gitlink,
CMake integration, helper overlays, tests, and parity manifest as one reviewed
change.

`docs/ghostty-parity.json` binds every tracked configuration key, keybinding
action, and CLI action to that revision. `scripts/check-ghostty-parity.py`
re-extracts upstream inventories and rejects an inconsistent pin or
unclassified drift.

## Verification boundaries

CTest covers project value types, the adapter, PTY sessions, renderer,
workspace, configuration, keybindings, D-Bus/portal protocols, staged
installation, and full-process lifecycle. Tests use private D-Bus services,
fake systemd/cgroup state, temporary configuration homes, and deterministic
font/resources where external state would otherwise make results unstable.

Software scene-graph tests provide deterministic headless coverage. Wayland
OpenGL/Vulkan tests and the renderer qualification tools cover the RHI path;
final compositor behavior remains a host-level validation boundary.

Run and filter tests using the workflows in
[Development and CI](development.md). Future performance experiments and their
benchmark and GPU-qualification entry points are documented separately in
[Performance](performance.md).
