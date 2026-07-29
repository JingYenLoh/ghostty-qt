# Architecture

## Scope and stack

`ghostty-qt` is intentionally Linux- and Wayland-only. Its first milestone is a
usable terminal core with tabs and splits, while keeping the boundary between
the Qt application and Ghostty's terminal engine small enough to evolve.

| Layer | Technology | Responsibility |
| --- | --- | --- |
| Window and chrome | Qt Quick, QML, Quick Controls 2 | Window, tab strip, toolbar, and confirmation dialogs. |
| Terminal item and workspace | C++23, Qt Quick | Scene-graph rendering, input events, focus, tabs, and a recursive split tree. |
| Audible bell | Qt Widgets platform integration and Qt Multimedia | Best-effort Wayland system bells plus reusable per-pane custom audio playback. |
| Session orchestration | Qt Core/Gui on a dedicated `QThread` per started pane | PTY I/O, child lifecycle, immutable frame snapshots, and queued UI communication. |
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

The process controller owns every primary window; each terminal pane then has
the following ownership and data flow:

```text
ghostty-qt raw CLI classifier (before Qt)
  -> exec sibling ghostty-qt-config-helper for the supported +action subset
  -> QCore-only +new-window/+toggle-quick-terminal client
     -> org.gtk.Actions on the session/starter D-Bus

ApplicationController (UI thread, process lifetime)
  <- SingleInstanceActivation
     <- org.freedesktop.Application and org.gtk.Actions at one object path
  -> reusable Main.qml component -> zero or more primary windows
     -> TerminalWorkspace (UI thread, recursive tabs/splits)
        -> TerminalPane (QQuickItem with scene-graph contents)
           -> TerminalController (UI thread)
              -> on accepted session start:
                 <queued signals>
                 -> SessionWorker (one dedicated QThread)
                 -> GhosttyVtAdapter
                    -> libghostty-vt terminal/render/input handles
                 -> PTY master <-> child process group
  -> ApplicationLifetimeController
  -> GhosttyApplicationKeybindings

GhosttyConfigService (UI thread)
  -> ghostty-qt-config-helper (short-lived child process)
     -> pinned ghostty-internal configuration/CLI implementation

GhosttyApplicationKeybindings (UI thread, process lifetime)
  -> compile one immutable keybinding program per configuration generation
     -> ApplicationController -> TerminalWorkspace -> TerminalPane state
  -> root application-action pre-pass
  -> all/global action-major fanout -> registered TerminalWorkspace instances
  -> XDG Global Shortcuts portal (session D-Bus)
```

`LaunchOptions` remains on the application, workspace, and pane side because it
also owns font policy, keybindings, close behavior, link previews, the effective
working-directory, future-surface inheritance, and tab-strip presentation
policies, and a compact
`SplitAppearance` value containing unfocused-pane opacity/fill and the optional
divider color. Split appearance stays
frontend-owned and never crosses the session-worker boundary. Before a
session starts, the pane projects `LaunchOptions` to
`TerminalSessionLaunchOptions`, containing only the child process, scrollback,
terminal identity, configured environment overrides, appearance, URL-matching
state, and an optional one-shot initial geometry owned by the session. The
geometry is not produced by the reusable `LaunchOptions`
projection: a workspace supplies it explicitly only while constructing its
first pane. Before session start, the controller folds the newest runtime
values into that pending launch state. Later live reloads cross the queued
controller/worker boundary as `TerminalSessionRuntimeOptions`, which is limited
to appearance and URL matching. Terminal identity, configured environment
overrides, Linux cgroup policy and limits, working directory, program, hold,
and the existing terminal's scrollback allocation are launch-only by
construction. A pane snapshots those values when constructed, so even a
deferred pane that has not spawned its child retains its original identity,
environment, and resource policy across reload; panes constructed afterward
use the newest workspace snapshot.
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
One public `QSGSimpleRectNode` paints each visible handle. A configured RGB
uses that exact color; an unset value uses the frontend's opaque ordinary gap
color instead of exposing the desktop through the transparent terminal host.
Color reloads update existing handles in place, while newly created handles
inherit the current workspace value. Drag ratios remain unitless, clamp to
`[0, 1]`, and relayout only after the floored divider position changes. The
handles accept no keyboard focus, so active-pane and search-overlay focus
survive a drag. Moving the workspace to a different Qt Quick scene destroys
the old scene's handles synchronously to release any delivery-agent grab, then
recreates them from the stable split IDs.

Zoom changes only presentation: the complete tree, ratios, PTYs, and logical
geometry remain intact, while divider handles are absent. Only the current
tab's panes and dividers are exposed. The active pane supplies the tab's base
title and receives toolbar and directional-focus actions. Each controller
stores that base as an optional value so absence selects the launch-program or
`Terminal` fallback while `set_surface_title:` remains a present empty title.
Each pane separately stores the optional manual override produced by
`prompt_surface_title`; effective surface display is override, then base, then
the Qt fallback. Raw surface consumers stop before the fallback, and a tab
override remains a still-higher presentation layer.
The parameterized action decodes the structured helper's canonical escaped
UTF-8, requires the originating stable `PaneId`, and mutates only this
GUI-thread cache. A later worker title event replaces it; comparing the full
optional state ensures OSC can restore the exact string cached before the
action. An empty OSC update returns to absence and the fallback. Reset
preserves the current base title regardless of its latest writer because the
pinned Ghostty action publishes no application title update. All/global fanout
uses the existing stable pane snapshot and neither activates a pane nor touches
terminal selection. The strict void `prompt_surface_title` snapshots the raw
surface override or base by stable `PaneId`, excluding tab and fallback text.
Non-empty confirmation preserves the exact text as a persistent override;
empty confirmation clears it, and Cancel is inert. Base updates remain cached
while masked, so clearing reveals the newest OSC or action value. The strict
void `copy_title_to_clipboard` consumes the same raw effective surface value;
absence and explicit empty return false, while non-empty Unicode is written
exactly to the standard clipboard on the GUI thread. It never consumes the tab
override, zoom/fallback presentation, primary selection, or terminal
selection lifecycle. Broad fanout retains the existing stable tab/tree order,
so its last non-empty pane is the final standard-clipboard writer. This
frontend order differs from Ghostty's process-wide creation/swap-remove
surface vector after reordering or deletion. A non-empty
`set_tab_title` payload installs a per-tab display-title override by stable tab
identity; an empty payload clears it. Pane focus changes and OSC title updates
continue to update the base title without replacing the override, so clearing
it reveals the then-current active-pane title. Tab insertion and reordering do
not redirect or discard the override. The strict void `prompt_tab_title`
action captures that source tab's stable identity and its raw override, or the
current display-title snapshot when no override exists. The latter includes
the modeled zoom prefix; an override is presented raw. Its modal QML text
field takes focus with the caret at the end and does not select all. OK applies
the exact entered text through `set_tab_title`, including surrounding spaces;
blank therefore clears the override, while Cancel performs no mutation.

BEL presentation is a separate, pane-local latch above those raw title layers.
The worker publishes every Ghostty bell through `TerminalController`; the pane
sets its latch on the first event but continues publishing an every-event
notification so repeated BELs can renew host-window attention. Gaining
terminal focus, a non-modifier key press, an IME commit/preedit transition, or
any terminal mouse press clears only that pane. Key releases and
modifier-only presses do not clear it. The active pane's latched state drives
the optional `🔔` display prefix before either its terminal-derived title or
the tab override, without mutating the controller base title, surface
override, or tab override consumed by title actions. A bell in another split
therefore retains that split's independent border state without replacing the
active surface's title.

Tab attention is intentionally distinct from the pane latch, matching
Ghostty's GTK page behavior. A BEL from any pane of an inactive tab marks that
stable `TabId` as needing attention; selecting the tab clears the marker, then
ordinary focus transfer clears the selected active pane's latch. A BEL never
marks the already selected tab, even if its host window is inactive. The
`attention` bell feature instead gates a native `QWindow::alert(0)` request for
an inactive host window; Qt ignores the call for an active window and ends an
indefinite alert when that window becomes active.

Audible effects consume the same every-event publication rather than the
transition-only latch. `system` calls the public `QApplication::beep()` route;
on Wayland the result is deliberately best-effort because Qt and the compositor
must expose a platform bell. `audio` requires a finalized
`bell-audio-path`. Each pane owns one `TerminalBellPlayer`, which lazily owns
one `QMediaPlayer` and `QAudioOutput`; separate panes can overlap, while a
repeated BEL in one pane stops and restarts that pane's cached source. The
required/optional bit participates in source identity, and volume is clamped
only at playback. An inaccessible required path warns once per unchanged
identity but is retried on every BEL; optional failures remain quiet. An
asynchronously invalid medium clears the ready cache so a later BEL rebuilds
it.

The pane snapshots the complete bell options once per event. It then publishes
the latch and workspace notification before invoking a temporary shared player
lease, without touching the pane afterward. Destructive synchronous title,
tab, attention, or workspace observers therefore cannot leave playback using a
destroyed pane. Live feature, path, provenance, and volume changes affect the
next BEL only; they neither clear the latch nor interrupt an already-playing
source.

A successful `goto_split` first resolves its destination, then applies focus
and zoom as one workspace transition. The canonical
`split-preserve-zoom = navigation` policy transfers an existing zoom to that
destination; the canonical `no-navigation` default clears it. A direction with
no destination changes neither focus nor zoom. Direct activation of a
different pane still clears zoom. A new split can be placed on any side of its
source; left/up insert the new leaf before the source and right/down insert it
after. The omitted or explicit `auto` direction compares the source pane's
effective surface-pixel width and height, choosing right only when width is
greater and down otherwise. Every split focuses the new leaf and clears split
zoom.

Tabs and panes have monotonically assigned `TabId` and `PaneId` values. The
workspace resolves those identities at execution time instead of retaining
vector rows or raw pane pointers across deferred operations. A
`QAbstractListModel` publishes tab identity, effective title, raw title
override, active pane, working directory, running state, zoom, attention,
progress, and read-only roles to QML. The current tab strip consumes that
model. Updating or clearing an override, or changing the active surface's
title-bell presentation, notifies the title role without changing either raw
title field. The attention role drives the inactive-tab emphasis described
above. The read-only role follows each tab's active pane; the pane also exposes
the state directly for its visible status badge. Progress remains a foundation
for later parity work rather than a user-visible feature today. The workspace
owns all structural model mutation: C++ consumers receive a const typed view
and QML receives an abstract model facade, so synchronous observers cannot
desynchronize model rows from the pane topology.

Workspace commands pass through a typed `WorkspaceActionDispatcher` with an
explicit tab/pane context. Keyboard, pane, and QML entry points can therefore
converge on the same action vocabulary as more Ghostty keybindings are added.
Pending close and unsafe-paste operations retain stable IDs, so a model row
moving before confirmation cannot redirect the operation to another pane or
tab. A surface close retains both its originating `PaneId` and containing
`TabId`; active split removal selects the previous tree-order leaf unless the
target was leftmost, in which case it selects the next leaf, while inactive
removal preserves focus. Removing a final leaf flows through the ordinary tab
and final-window shutdown paths. Close dialogs also carry nonzero request IDs,
so a delayed answer cannot resolve a newer request. Resolution and the approved
topology change share one guarded transaction, preventing synchronous
observers from inserting or closing a different target between those phases.
A pending tab-set close retains its originating
`TabId` plus frozen target membership; committing it starts every target worker
before the row-removal phase, removes the surviving members in current reverse
visual order, and restores focus to a surviving stable tab. Post-removal and
count observers see a coherent selection, and synchronous observers cannot
nest another topology action inside that commit. Broad unsafe
paste batches every stable target behind one confirmation. Broad actions that
close their own source converge on one confirmation-aware shutdown request per
workspace, while `close_tab:other` and `close_tab:right` keep ordinary
per-surface fanout and its stable source order. Window-close approval is an
irreversible workspace lifecycle state: subsequent structural typed workspace
actions are rejected, so no new worker can appear after the shutdown sweep.
An application-quit escalation, non-structural application action, or
pane-local step remains eligible to finish the Ghostty action chain that
approved the close. Application quit is sticky for the duration of a close
transaction: it upgrades an existing window confirmation in place, replaces a
narrower pane/tab confirmation with a freshly correlated window request, and
survives final-tab convergence. Cancelling that window request clears the quit
intent before publishing dialog resolution so a synchronous newer quit wins.
Quit requests received during a guarded topology commit latch immediately and
reconcile on the next event turn if that commit does not itself close the
window.

One typed title-prompt FIFO retains a stable `PaneId` for surface prompts or
`TabId` for tab prompts plus a nonzero request identity. Pane removal cancels
its surface prompts but leaves a tab request alive when the containing tab
survives; tab removal prunes that tab's queued requests. Removing an active
target resolves its dialog and advances asynchronously. A completion carrying
a stale request ID cannot rename a replacement pane, model row, or current tab.

Close policy tracks the live child separately from active foreground work. For
an interactive shell, `tcgetpgrp` detects jobs in a separate foreground process
group, with a short conservative latch around command submission; an explicitly
launched program is active for its whole lifetime. The launch transformer
retains whether a shell integration was successfully installed, so startup
remains active until that shell emits its first prompt. The worker queries
libghostty's public active-screen row/cell semantic metadata after each PTY
batch and on its process timer, sampling and latching `AtPrompt` even while the
foreground-group or submission latch has active precedence. `AtPrompt` clears
activity after that precedence ends, while `Away` protects same-process-group
shell builtins and alternate-screen work until the next prompt.
`Unavailable` also remains active once integration is expected or a prompt has
been observed, so a transient public-query failure cannot approve a pending
close. A shell for which integration was not installed retains the historical
process-group/latch fallback. The public API does not expose Ghostty's private
live cursor semantic mode or a terminal-owned integration-seen bit. `always`
protects any live child, and neither mode prompts after exit. Pending dialogs
are re-evaluated when state changes.
Destruction sends
`SIGHUP` to the child's process group, allows a two-second grace period, and
uses `SIGKILL` if the group does not exit; workspace/tab teardown starts all
pane shutdowns first so grace periods overlap. A close-on-exec readiness pipe
holds parent-side publication until the `forkpty` child has reset its inherited
`SIGHUP` disposition, so an immediate close cannot enter an application signal
handler in the child before `exec`.

When the finalized Linux cgroup policy applies, a second close-on-exec
parent/child gate extends that boundary. The child publishes PTY readiness and
then performs only a blocking syscall read; it cannot `chdir` or `exec` until
the worker thread has attempted placement through the user systemd manager and
either verified the expected scope leaf through `/proc/<pid>/cgroup` or applied
the configured soft-failure fallback. The exact transient unit is
`app-ghostty-surface-transient-<pid>.scope`, requested with mode `fail`,
optional `MemoryHigh` and `TasksMax` uint64 properties, always
`ManagedOOMMemoryPressure=kill`, and the child PID. Membership is sampled every
25 ms for at most 250 ms. A soft failure releases the child with a warning; a
hard failure sends a rejection byte, reaps the child before returning, and
reports status 127. Failure to allocate the gate before `forkpty` follows the
same soft/hard policy; a post-fork socket/protocol failure cannot safely release
the child and is always fatal. The child side invokes no Qt or D-Bus code after
`forkpty`.

`linux-cgroup=single-instance` consumes a process fact established by startup
name arbitration: it is true only for the retained primary activation endpoint.
That fact remains fixed when either configuration domain reloads, while the
cgroup mode and limits continue to update future pane snapshots. Scope cleanup
requires no `StopUnit`; systemd owns the transient scope until the pane's last
descendant exits. The `/proc` parser intentionally follows pinned Ghostty's
first-entry contract; a unified cgroup-v2 hierarchy is the primary verified
environment, while a legacy first entry with the expected scope leaf is also
accepted.

Process lifetime is application-owned rather than a side effect of QML window
destruction. Qt's implicit `quitOnLastWindowClosed` behavior is disabled.
`ApplicationController` maintains a `QPointer` registry of C++-owned QML roots,
and its embedded `ApplicationLifetimeController` registers each primary window.
Retiring a non-final root leaves process lifetime untouched; retiring the final
root either keeps the process resident, queues an immediate exit for the next
event turn, or starts one single-shot `QChronoTimer`. Successfully presenting a
replacement cancels an active timer, while factory failure leaves it armed.
Transient dialogs are not registered. Each approved root and its workspace are
deleted after closing, so delayed or resident operation retains no dead panes
or controller threads.

`new_window`, `open_config`, `reload_config`, and `quit` use a typed
process-action vocabulary that remains available with zero workspaces. Window
creation is queued to the GUI event loop, reuses one `QQmlComponent`,
resolves the latest per-window options, and validates the still-hidden root.
Pane-originated and source-less focused-window requests assign that source
screen before sizing; an initial or zero-window request retains Qt's primary
screen default.

Before `QApplication` or any `QQuickWindow` exists, startup unconditionally
requests Qt's default alpha buffer. The capability is therefore available even
when the initial opacity is `1`, so a later reload can become translucent
without recreating a native window. `Main.qml` uses a transparent
`ApplicationWindow` clear color and places `TerminalWorkspace` directly in
that host, allowing each pane's alpha-bearing background to reach the
compositor. The retained top or bottom toolbar slot remains an opaque
rectangle, and unset split dividers paint an explicit opaque frontend fallback,
so terminal transparency does not make application chrome or the interactive
two-pixel split gaps translucent.

`goto_window` keeps Ghostty's less obvious surface scope: each matched surface
emits a typed previous/next request, then `ApplicationController` traverses its
live root registry because only the process owner can see every window. The
currently active registered root is the starting node; when none is active,
the first registration is tested first. Traversal follows registration order,
wraps once, and skips the active root, hidden roots, close-committed
workspaces, and workspaces without an active pane. A minimized destination is
restored without discarding a simultaneous maximized/fullscreen state, then
activation is requested synchronously so the Wayland backend can retain the
originating input serial, and its active pane receives Qt focus. Guarded
window/workspace pointers keep reentrant traversal and callback-scheduled
retirement from retaining stale registry objects. The compositor still owns
the final decision to grant activation. Because the action remains
surface-scoped, `all:` and `global:` deliberately perform one traversal per
visited surface, matching Ghostty's action-major fanout instead of collapsing
to one process action.

When both configured grid dimensions are nonzero, the controller uses the same
Qt font construction and integral cell metrics as `TerminalPane`, adds the
QML-declared persistent chrome, applies the 10-by-4 cell minimum, and clamps
the requested logical-pixel client size best-effort to the available screen.
It performs this resize before workspace initialization, so the first pane is
laid out at the resulting grid, and revalidates the guarded root/workspace pair
because resize observers are synchronous. It then initializes the workspace,
seeds its pre-fullscreen restore state, retains the hidden normal size when the
first map will be maximized/fullscreen, and requests its initial
normal, maximized, or fullscreen Qt window state through the activation-aware
presentation call. Qt Quick exposes only the dominant fullscreen state, so
when both settings are enabled the retained state and a visibility observer
restore maximized after either an action- or compositor-driven fullscreen
exit. A separate session-start transaction consumes the selected initial
command and CLI hold state only when the first session successfully initializes
libghostty-vt. A process-wide coordinator grants immutable positional-program,
tagged-command, and hold snapshots in FIFO start order, commits a grant before
child launch, and releases it if terminal initialization fails. Positional CLI
argv wins over the configured `initial-command`, which in turn replaces
`command` for this one lease. Suppressed startup, window construction or
presentation failure before terminal initialization, and a deferred root
closed before exposure therefore retain the current values for the next
eligible session. Reload may replace the pending tagged command before a pane
reserves the lease; an existing holder keeps its immutable snapshot, and a
reload cannot re-arm the command after consumption. A later child-launch
failure does not restore a committed grant; later sessions use the ordinary
configured command or finalized default shell. A
surface source is the composite live workspace plus stable `PaneId`, because
pane IDs are only workspace-local;
stale sources fall back to the focused or most recently active workspace, then
to process defaults. Explicit `quit` read-only-assesses every live workspace,
hosts at most one confirmation on the active window, changes no window on
cancel, re-hosts if that dialog window disappears, and begins every workspace
shutdown before emitting one irreversible process quit. It bypasses both the
disabled last-window policy and any delay, including with zero windows. A
command supplied after `--` matches Ghostty's `-e` lifetime rule by forcing
immediate last-window exit.

The strict void `open_config` action emits one process request even when a
broad binding originated from multiple surfaces. On Linux its edit candidates
are the preferred XDG `config.ghostty` followed by legacy `config`, deliberately
the reverse of their configuration merge order. The first non-empty file wins;
otherwise the first existing empty file wins, and if neither exists the
preferred file and its parent directories are created. Candidate inspection
uses native open/stat errors, and creation is exclusive so an existing or
concurrently created file is never truncated. Qt receives the selected path as
a `QUrl::fromLocalFile` desktop-open request. Preparation or desktop-launch
failure is logged without turning the consumed application binding into PTY
input. This GUI action is separate from Ghostty's `+edit-config` CLI action,
which has editor-environment and path-selection semantics of its own.

The nullable delay crosses the structured config boundary after Ghostty's own
`Duration.asMilliseconds()` conversion, preserving configured zero separately
from null, truncating sub-millisecond components, and saturating at `c_uint`.
Successful live reloads compare the two effective policy values: an identical
reload preserves the current deadline, while a changed value cancels and
reconciles it against the current window state. This deliberately repairs the
pinned GTK frontend's stale-timer reload edge cases. The false policy can keep
a zero-window application resident; process and portal actions can reload it
or construct another window. Independently, the pinned GTK runtime does not
arm this timer before any surface has ever been requested: its generic startup
hook is conditional on a method that the GTK `App` does not expose. The
controller therefore preserves the same never-requested state for
`initial-window=false`; last-window policy begins only after a real surface has
existed and closed. This differs from the prose comment on the pinned config
field, but matches its executable GTK control flow.

Linux process and desktop activation share the canonical
`org.freedesktop.Application` session-D-Bus endpoint. Its well-known name is
the build's application ID and its object path is derived by replacing dots
with slashes and hyphens with underscores. Debug appends `.Debug` consistently
to the runtime ID, desktop filename, service filename, and derived path, so a
developer build cannot activate an installed Release build. The object is
exported before an atomic, non-queued, non-replaceable name claim, preventing a
cold request from observing an owner without its endpoint. `Activate(a{sv})`
is functional. Standard `ActivateAction(s,av,a{sv})` and a sibling exact
`org.gtk.Actions.Activate(s,av,a{sv})` accept typed `new-window`,
`new-window-command`, and `toggle-quick-terminal` requests. Unknown names and
malformed parameter variants are rejected before entering the queue.
`Open(as,a{sv})` remains exported with a stable `NotSupported` error.

The launching process's typed `initial-window` decision determines what
happens when another owner exists: true sends standard source-less activation,
while false unregisters its temporary endpoint and exits normally without
contacting the owner. Ordinary bare activation forwards no argv, cwd, shell
text, or general environment state. Its sole launcher-state exception is a typed
`DesktopActivationContext`: exact string-valued `activation-token` and
`desktop-startup-id` fields are retained with each request, while unknown
fields, wrong variant types, and embedded NULs are discarded. Pending startup
requests keep their message/context pairs in FIFO order. A process carrying
`DBUS_STARTER_ADDRESS` connects through Qt's activation-bus alias; an ordinary
process uses the session bus. Calls arriving before startup finishes retain
delayed replies. Both action interfaces and bare activation share one bounded
FIFO. The handler is installed only after controller, configuration, lifetime,
and test wiring, and standard-interface success is acknowledged after
synchronous window registration. The GTK interface preserves `GAction`'s void
contract: a well-formed request is acknowledged after handler dispatch even
when the UI operation reports failure.

The `+new-window` and `+toggle-quick-terminal` clients branch before ordinary
Qt option parsing, `QGuiApplication`, configuration loading, or native-surface
creation. A minimal `QCoreApplication` exists only for the blocking D-Bus call.
There is exactly one call, normal bus service activation is allowed, and an
error never falls back to constructing a local window. The new-window sender
operates on raw argv bytes: it validates UTF-8, strips pre-`-e` class targeting,
canonicalizes concrete relative or tilde working directories, and preserves
Ghostty's implicit caller-cwd plus later home/inherit quirk. After the
top-level detector has enforced one `+` action, the action parser makes every
post-`-e` argument opaque and sends one `as` variant to
`new-window-command`. The toggle sender deliberately ignores all non-action
operands and targets the default build application ID, matching the pinned
executable rather than its broader help text.

The receiver recognizes only command, working-directory, title, and `-e`
forms; successful repeated values are last-wins and unknown options are inert.
Direct argv after `-e` overrides an earlier command only when non-empty.
Decoded values enter `TerminalWorkspace` as a one-shot creation payload and
are never stored as workspace defaults. The requested command still reserves
the process-wide initial-session lease; after lease arbitration it overrides
the first pane's program and hold values. A concrete directory, or the pinned
receiver's literal relative path text `home`/`inherit`, and an
explicit-empty-capable persistent surface title are likewise installed before
that pane is published. Later tabs, splits, and windows use the ordinary
effective launch policy.

Each registered top-level receives a monotonic `WindowId`; pane IDs remain
workspace-local. The command palette retains configured commands as typed
strings but rebuilds its runtime rows immediately before every opening by
sampling committed pane trees from every window. A live-surface row stores
`SurfaceTarget { WindowId, PaneId }`, never a `QObject` pointer or serialized
QML action. Its label uses the raw effective surface title, with `Untitled`
only for absence and not for an explicit empty title; cwd is shown only when it
is non-empty and not already contained case-sensitively in that title. The
combined model uses the existing colon-normalized case-folded order, with the
composite identity as the deterministic tie-breaker for equal focus labels.
Activation captures the typed value before modal teardown, re-resolves both
IDs, selects the exact tab and split, then presents or deiconifies its normal
window or shows the retained hidden quick terminal. Closing either identity
between snapshot and invocation makes the row inert.

For a direct launch, `main` captures and unsets `XDG_ACTIVATION_TOKEN` and
`DESKTOP_STARTUP_ID` before constructing `QGuiApplication`; an existing-owner
fallback serializes those values into the standard platform dictionary, while
a new primary carries them directly to its first window. Immediately around
the target `QWindow::show()`, the GUI thread projects the typed values back to
those two conventional variables so Qt's Wayland platform plugin can consume
the transferred token. Immediately before projection, the activation state
caches a sanitized child environment and marks presentation active. While it
is active, `SessionWorker` returns that cache instead of walking process-global
`environ`; outside presentation it takes a fresh sanitized snapshot under the
same state mutex. This avoids racing Qt Wayland's own internal token unset and
also lets a synchronously destroyed workspace join its worker without
deadlocking on `show()`. The sanitization removes both names from the inherited
child base; a finalized `env` override is deliberately later and may explicitly
reintroduce either name without recovering the consumed activation value. An
RAII guard always clears the one-shot variables afterward and never restores a
consumed value. A window shown with transferred platform data does not issue a
second `requestActivate()`.

Window creation is a checked ownership transaction. The workspace must be
both visually associated with the root and inside its QObject ownership tree;
guarded liveness checkpoints follow initialization, lifetime registration,
presentation, optional activation, and creation observers. Those window
operations do not themselves consume the first-session lease, but an immediate
pane can initialize while the presentation transaction is still running. Once
a session successfully initializes libghostty-vt, its lease remains committed
even if child launch fails or a later callback destroys the pair; a failure
before that boundary releases or never requests it. Any window-creation failure
tears down the remaining half,
and standalone workspace destruction retires its now-invalid root on the next
event turn before normal last-window policy runs. Nested synchronous
creation is rejected until the transaction completes, and every checkpoint
revalidates both ownership relationships rather than checking only liveness.

An unavailable bus starts independently. On a connected bus, rejection or
incompatibility from the same live owner is fatal, while a bounded owner-
identity loop follows a replacement owner during handoff. An ambiguous
no-reply fails closed because the owner may already have created the window.
This creation acknowledgement deliberately strengthens pinned Ghostty's
fire-and-forget normal activation.

The private structured config export retains Ghostty's boolean
`initial-window` value and raw `gtk-single-instance` false/true/detect enum.
The latter is an unused field in the sole accepted schema-v1 contract: process
uniqueness belongs to the independent frontend `single-instance` setting. The
GUI process resolves frontend `detect` from its real invocation and
`TERM_PROGRAM`. Parsing
records whether an invocation has unforwarded window/session payload, so the
exact `--single-instance` and `--initial-window` coordination flags remain
eligible while cwd, font, hold, scrollback, and program arguments stay
independent. Explicit coordination values outrank both configuration domains on
initial load and reload.

Role and name ownership stay fixed across live reload, matching
GApplication construction-time policy. A fresh launcher samples its own latest
frontend setting. Reloading `initial-window` changes no current window. The
primary activation handler itself remains unconditional, so a true launcher
can create the first window in a primary that began false. An accepted
activation creates synchronously from the primary's latest process options, and
its pane participates in the same process-wide first-session lease. Only the
first session that successfully initializes receives one-shot program and hold.
The activation path overlays only an actually focused or still-valid
last-focused pane cwd when configured, and keeps the configured font size. With
no valid focus it keeps configured cwd rather than choosing an arbitrary live
pane. That cwd/font asymmetry matches the pinned GTK null-parent path.

## Output path

1. `SessionWorker` creates a `GhosttyVtAdapter`, which owns the Ghostty terminal
   and render-state handles, then starts the selected tagged or positional
   command with `forkpty`.
2. A nonblocking `QSocketNotifier` drains the PTY master and sends bytes to
   the adapter's VT-write operation.
3. Ghostty applies VT state changes and reports deferred effects such as title,
   working directory, and bell notifications.
4. Screen updates are coalesced on an 8 ms single-shot timer. First frames,
   resize, viewport scroll, and Ghostty full-dirty states produce a value-only
   full-grid update. Ordinary output copies only Ghostty's indexed dirty rows;
   colors, the effective 256-entry palette, cursor state, scrollbar metadata,
   and each cell's OSC 8 presence bit cross as values. Non-virtual Kitty
   placements and their decoded pixels are copied into immutable,
   generation-stamped Qt snapshots while every libghostty handle is still
   valid on the worker thread. Each published update also carries the worker's
   current terminal-content revision.
5. A queued Qt signal copies the update across the thread boundary. The pane
   validates and transactionally merges it into its retained frame under a
   mutex, then schedules a scene-graph update. Dirty state is cleared only
   after the adapter successfully copies the complete update.
6. `TerminalPane::updatePaintNode()` keeps fixed before-text/main-text/after-text
   groups, three persistent Kitty-graphics insertion points, and frontend
   overlays followed by one persistent full-pane unfocused-split rectangle.
   The image insertion points are ordered below cell backgrounds, between cell
   backgrounds and terminal foreground content, and above terminal foreground
   content but below input-method and pane overlays. Ordinary Kitty placements
   populate those points from the retained value snapshot. Textures are keyed
   by libghostty's process-unique image generation, so metadata updates,
   scrolling, and placement-only mutations rebuild at most geometry; replacing
   an image ID uploads the new generation and deletion evicts it.
   Main terminal text is retained in one public `QSGTextNode` per visible row;
   accepted row epochs rebuild only changed rows, while font, geometry,
   appearance, palette, search, and frame-shape changes rebuild the complete
   text layer. Old and new block-cursor rows are rebuilt when its text override
   changes. The row nodes use `QtRendering`, which stores distance-field glyphs
   in GPU atlases on hardware RHI backends. The final rectangle keeps an empty
   extent while inactive and updates without scene-graph allocation. Dimming
   state is absent from the retained text-state key. Existing focus-driven
   block-cursor changes keep their targeted row rebuilds, while search
   decoration changes retain their full text-state invalidation.
7. Cell-derived backgrounds, resolved glyph/decor colors, and before/after-text
   decorations are retained in one render-thread cache per visible row.
   Independent solid-row epochs cover terminal dirty rows; search and
   hyperlink mask replacement marks only rows whose bits changed. A global
   solid-state key invalidates all rows for palette, appearance,
   cell-affecting opacity, geometry, or device-pixel changes. Block-cursor
   transitions rebuild only their old and new rows, while bar/underline cursor
   movement and metadata/frontend-overlay updates reuse every row plan. Each
   row owns three persistent `TerminalRectBatch` instances on RHI backends, in
   painter-ordered background, before-text-decoration, and
   after-text-decoration containers. Only rebuilt rows begin and commit those
   batches. Padding, cursor, terminal overlay, and pane overlay geometry
   remains in small global batches. The software adaptation deliberately
   flattens cached rows into its three global `QSGSimpleRectNode` pools,
   avoiding the traversal cost of hundreds of empty row nodes where there is
   no GPU upload to save. Each batch's two CPU vectors exchange storage instead
   of copying, identical batches do not dirty the scene graph, and RHI geometry
   grows only when its retained capacity is insufficient. Each nonempty
   cell in a rebuilt text row is shaped with `QTextLayout` and placed at an
   explicit grid coordinate. This avoids fallback-font and wide-cell advances
   shifting later cells. Cell values retain foreground provenance, a separate
   explicit-background bit, and bold, faint, inverse, invisible, underline,
   strike-through, overline, and text-blink attributes so frontend-only
   appearance rules do not have to be flattened at the worker boundary. The
   GUI selects one of four cached regular, bold, italic, and bold-italic Qt
   faces from those attributes; the worker never owns a font or platform
   font-database handle.

The adapter derives explicit-background provenance from the public
`GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR` query rather than comparing
resolved colors. A successful query therefore remains explicit even when its
RGB is identical to the terminal's global background, and covers both styled
SGR cells and background-only cells produced by erase operations;
`GHOSTTY_INVALID_VALUE` means the cell uses the default background. The bit is
retained independently of inverse, selection, and search presentation so the
renderer can apply those later policies without reconstructing terminal style
state.

ENQ (`0x05`) follows a terminal-protocol return path inside that same worker
transaction. The adapter owns Ghostty's finalized raw `enquiry-response` bytes
and keeps that storage stable across the enquiry callback return and
libghostty's synchronous consumption. An empty response is silent. For a
response of 1–255 bytes, each parsed ENQ invokes the callback once and
libghostty forwards one byte-exact write—including embedded NUL—through its
ordinary `write_pty` callback. `SessionWorker` queues that callback with
device-status and other terminal-generated replies rather than with surface
input, so read-only mode does not suppress it. Live configuration replacement
happens on the session thread before later VT parsing, keeping the callback
storage and response generation ordered. The pinned public libghostty bridge
uses a 256-byte scratch buffer and silently drops responses of 256 bytes or
more; full Ghostty's termio path has no corresponding configured-length limit.

The application-facing adapter header contains only Qt and project value
types; the Ghostty C header and every Ghostty handle remain in its private
implementation. No Ghostty handle crosses into `SessionWorker` or the
UI/render side. The adapter returns value updates and deferred effects, making
ownership explicit and localizing future upstream C API changes. A full-grid
fallback keeps resize and viewport changes simple while ordinary output avoids
copying unchanged rows between threads.

The finalized `scroll-to-bottom.output` flag is evaluated at the frame
boundary, before the visible value update is copied. When output scrolling is
enabled and synchronized output is not active, the adapter resolves the active
screen's physical bottom grid node and row. A change from its retained
node-and-row anchor scrolls the viewport to the active screen before rendering.
The comparison deliberately ignores content and column: a same-line rewrite,
title change, or BEL does not jump, while adding or removing the physical final
row or changing screens does. Synchronized output defers both the comparison
and scroll until an eligible frame. The anchor is sampled only while the
feature is enabled, matching pinned Ghostty. Consequently, disabling the flag
prevents output-driven scrolling without advancing the anchor; enabling it
live after intervening output may scroll on the first eligible frame when that
stale anchor is compared.

Renderer-v1 uses Qt's public scene-graph API throughout: text nodes supply the
GPU glyph path and one retained vertex-colored `QSGGeometryNode` per painter
layer supplies solid primitives on RHI backends. Qt's software adaptation does
not render that public vertex-color material, so the test/fallback path retains
one reusable `QSGSimpleRectNode` pool per layer for correctness.
No intermediate raster-image upload sits between the frame and the scene
graph. Qt's implicitly shared frame snapshot is normally an O(1) reference-count
operation rather than a deep cell copy. During ordinary sparse updates, the
renderer resolves solid presentation and shapes text only for rows whose
persistent epochs or derived block-cursor state changed. Metadata-only,
frontend-overlay, and non-block cursor updates perform no cell-presentation
scan. On an RHI backend, each cached solid row commits directly to its three
persistent painter-layer batches, so a sparse update rewrites only geometry for
rows whose presentation changed. The software fallback retains the row plans
but flattens them into global node pools. Padding, cursor, and frontend overlays
use independent global batches; unchanged batches skip geometry updates and
every batch reuses its CPU and scene-graph allocation capacity. Global
text-state changes, including search-decoration mask replacement, still rebuild
the complete text layer.

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
  flushed. The same successfully encoded event returns the viewport to the
  active screen when it is a non-modifier and the live finalized
  `scroll-to-bottom.keystroke` field is enabled. A separately encoded
  non-modifier current key qualifies after staged bytes are replayed, but
  replaying staged leaders alone does not.
- One typed IME value carries the commit and preedit transition through a
  single queued worker operation. Commits use the same encoded-key policy;
  preedit start/change/end clears independently when configured. The rendered
  preedit string remains a local UI overlay. A successfully encoded IME commit
  is likewise a non-modifier keystroke for `scroll-to-bottom`; preedit alone is
  not.
- The finalized default-false `vt-kam-allowed` policy also crosses that queued
  runtime boundary. After Qt has resolved keybindings, the worker queries
  libghostty's terminal-owned ANSI mode 2 and suppresses ordinary physical
  keys and committed IME text only when both states are true. Matching
  sequence leaders run inside Ghostty's pre-KAM keybinding path, so their
  encoded bytes remain staged and flushable even while mode 2 is active. A
  separately resolving current key is checked when its sequence resolves.
  Preedit lifecycle remains terminal-local. The worker also publishes mode-2
  transitions to the controller before the corresponding frame is scheduled;
  the pane uses that mirror to avoid KAM-rejected pointer-hiding and hyperlink
  modifier presentation, while the worker's direct terminal query remains the
  authoritative byte gate.
  CSI, ESC, and text binding actions plus paste retain their raw input paths
  and bypass KAM. Policy reload never mutates mode 2; DECRST takes effect
  immediately, while child-exit normalization clears KAM before a waiting
  surface accepts its dismissal key.
- Mouse events use Ghostty's mouse encoder when an application enables mouse
  tracking. The finalized four-state `mouse-shift-capture` value crosses the
  strict configuration wire and reloads into each existing pane. `always`
  keeps `Shift` in captured DEC button events and `never` releases captured
  button input for local handling; those terminal-override-independent
  capture-routing branches are exact.
  `true` and `false` currently select the same configured capture/release
  fallback, respectively. They cannot yet honor a program's later
  `XTSHIFTESCAPE` request because official libghostty-vt does not expose the
  terminal's private unset/false/true flag. The missing public query contract
  is tracked in `REQUIRES_UPSTREAM.md`.
- A local right press crosses to `SessionWorker` as a correlated value carrying
  its cell, retained-frame revision, normalized modifiers, and whether `Shift`
  released raw DEC capture, even when the `mouse-reporting` policy disables
  effective event routing. This keeps `copy-or-paste`, selection containment,
  and link/word selection atomic with the worker-owned terminal. Reported DEC
  input retains precedence. Each request remains independently correlated so
  rapid paste effects are not collapsed; the pane supersedes only obsolete
  context-menu presentation. For `context-menu`, a current click inside the
  installed selection preserves it; otherwise the worker selects an exact
  `Ctrl` OSC 8 cell, a complete configured-regex match, or the clicked word and
  then applies copy-on-select. A stale coordinate may still request the menu
  but cannot select unrelated current content. The GUI retains only the press
  positions and maps results to paste or a workspace-issued stable menu token.
  One Qt Quick `Menu` per window uses `Popup.Item` so its geometry stays in
  the same Qt scene on Wayland. Immediately before opening, QML maps the
  retained window-root click point into the `ApplicationWindow` content item,
  accounting for its header and footer. The menu exposes only Copy, Paste,
  Reset, a Split submenu containing Change Title, all four directional splits,
  and Close Split, a Tab submenu containing Change Tab Title, New Tab, and
  Close Tab, a Window submenu containing New Window and Close Window, and a
  Config submenu containing Open Configuration and Reload Configuration. QML
  retains the selected fixed action until the popup closes. The workspace then
  consumes the stable token once, restores focus to the current active pane,
  revalidates the stored `PaneId` and guarded originating pane, and dispatches
  the action there. Window-local dispatch therefore retains the originating
  pane's host while New Window and both configuration actions cross the
  established application-controller path. Restoring focus before dispatch
  ensures a resulting title or close confirmation opens last and retains focus.
  Dismissal only restores focus; cancellation when the origin disappears or a
  newer popup supersedes it is inert, and a consumed or stale token cannot
  dispatch again.
  The same live shift-capture decision is used for pointer button press and
  release, held-button drag motion, middle/right actions, and modifier
  normalization for `Ctrl+Shift` link hover and activation. Matching Ghostty,
  captured wheel/fractional scrolling and buttonless DEC-motion reporting do
  not consult this policy and remain reported. Buttonless `Ctrl+Shift` hover
  still uses the same decision for local hyperlink eligibility. Raw DEC
  capture remains the
  modifier-normalization authority even when the frontend's independent
  `mouse-reporting` policy suppresses effective reporting, so disabling
  reporting does not silently change raw-capture link/right-click modifier
  normalization.
- For local selection, Qt performs hit testing and forwards only typed press
  metadata: the cell, physical surface-relative pixel position,
  arbitrary-origin window-system timestamp, and modifiers. A nonzero Qt
  millisecond timestamp is converted to nanoseconds with checked
  multiplication; zero or overflow omits time from that press.
  `SessionWorker` supplies Ghostty's finalized `click-repeat-interval` and the
  current physical cell width to the reusable libghostty selection gesture.
  Libghostty accepts a repeat when the consecutive press-time difference is
  less than or equal to the interval and the Euclidean distance from the
  original first press is less than or equal to one cell width. It owns cell,
  word, and line behavior for single-, double-, and triple-clicks and clamps
  later repeats at triple. On Linux, `Ctrl` maps triple-click to OSC 133
  semantic-output selection; `Meta`/Super remains line selection. Qt's
  `MouseButtonDblClick` notification is ignored because the physical second
  press has already traversed the ordinary path. Ordinary left-button release
  preserves the gesture's time, original position, and count. A reported
  physical button clears selection and resets that history; reported motion
  does neither, while reported wheels and worker-rechecked captured fractional
  scrolling clear selection without resetting history. Live interval reload
  is sampled by the next press and does not reclassify an installed selection.
  When the effective shift-capture policy releases a Shift-left press and an
  installed selection plus gesture history exist, the adapter compares its
  typed timestamp with the mirrored prior ordinary-press timestamp. Only a
  comparable forward difference strictly greater than the live interval is an
  immediate drag; the inclusive boundary and missing or reversed timestamps
  traverse the ordinary press path. The drag retains libghostty's tracked
  anchor and selection behavior and does not replace the prior press time, so
  later delayed extensions still measure from that retained press. Linux
  `Ctrl+Alt` on a drag, or `Shift+Ctrl+Alt` on the delayed press, selects
  rectangle mode. The pointer arbiter places the matching crosshair below
  typing concealment and an accepted hyperlink pointer. It uses raw DEC state:
  `Ctrl+Alt` is sufficient normally, while raw mouse tracking additionally
  requires Shift, independently of the frontend reporting toggle and
  shift-capture policy. Below the crosshair, the same raw state selects an
  arrow normally or Ghostty's I-beam while Shift is held; without raw tracking,
  the I-beam is the base shape. Resetting the gesture also discards the mirrored
  timestamp. A valid drag that returns
  no range clears the installed selection, while an anchor belonging to an
  inactive screen leaves that screen's independent selection untouched.
  Hyperlink commit checks libghostty's release-stable dragged state as well,
  covering an immediate Shift extension that produced no GUI motion event.
- Vertical wheel input is normalized once on the GUI thread. A non-null Qt
  pixel delta is precision input;
  otherwise the angle delta remains fractional in 120-unit wheel ticks.
  Ghostty's independently finalized multipliers convert either form to pixel
  distance against the pane's current cell height. Each pane retains signed
  pending physical distance, so high-resolution input and direction reversals
  are not lost. Whole rows cross once as a value request carrying the
  pane-owned reporting policy, modifiers, and physical pointer position. The
  worker chooses against current ordered VT state: alternate screen plus no
  raw DEC mouse mode plus DECSET 1007 becomes one DECCKM-aware Up/Down sequence
  per row and clears selection; effective raw reporting becomes repeated DEC
  buttons 4/5; otherwise one typed viewport delta is applied. Read-only
  suppresses only the cursor-key or mouse bytes after terminal-local selection
  effects, while local viewport movement remains available. A live multiplier
  or cell metric change applies to new input while the already accumulated
  physical distance remains valid. Captured input queues a worker-rechecked
  selection clear even while its distance is below one row, without resetting
  repeat-click history. One event dispatches at most 10,000 rows and retains
  any excess distance. This makes synthesized extremes finite and keeps their
  repetition off the GUI thread. Pinned `Surface.zig` documents retaining the
  post-row remainder but currently subtracts the untruncated amount and clears
  it; this frontend intentionally follows that documented accumulator contract
  so high-resolution motion is not discarded.
- `mouse-hide-while-typing` is evaluated only after keybinding routing decides
  that a nonempty, non-repeating text press will reach the terminal. Ordinary
  pass-through keys, invalid-sequence flush-and-send-current, and an
  unavailable `performable` fallback can therefore hide the pointer; consumed
  bindings, staged leaders, modifiers, releases, raw actions, and paste cannot.
  A nonempty IME commit hides it through the same pane-local state, while
  preedit alone does not. Deferred keys and pending action chains retain the
  pointer-activity epoch from the original press. A late fallback may hide only
  if that epoch is still current, preventing old asynchronous results from
  undoing newer pointer, focus, or disabling activity. Enabling the policy
  advances the epoch without changing the presented cursor, so a key pressed
  while the policy was disabled cannot become retroactively eligible.
- Pointer motion reveals a typing-hidden cursor only after either physical
  axis moves by at least one device pixel; same-position and sub-pixel
  synthesized hover events are inert and do not advance the accepted position,
  so real high-resolution movement still accumulates to that threshold.
  Pointer leave, button press/release, wheel input, focus in/out, and live
  disabling reveal immediately and advance the activity epoch even when the
  cursor is already visible. One pane-local cursor arbiter then applies the
  strict priority blank-while-typing, resolved-hyperlink hand, rectangle
  crosshair, and raw-state-derived arrow/I-beam. Hiding never cancels a
  hyperlink lease, so revealing restores a still-valid link cursor. Public
  `libghostty-vt` does not expose its internally parsed OSC 22 mouse shape, so
  arbitrary application-requested W3C shapes remain an upstream-blocked
  extension rather than a parallel Qt escape parser. Its any-mode tracking
  boolean also loses full Ghostty's latest individual transition when multiple
  DEC mouse modes overlap; the same upstream effective-shape query is required
  for that uncommon ordering edge.
- `focus-follows-mouse` reuses that accepted physical-motion decision for
  hover and button-drag events. When enabled, an accepted move focuses the
  pane only if it does not already have active focus and its `QQuickWindow` is
  active. An inactive host is never activated or raised, reload alone never
  changes focus, and ignored same-position or sub-pixel events cannot select a
  different split. Focus-in publication may synchronously destroy the pane or
  workspace, so the pointer handler retains no raw continuation after calling
  `forceActiveFocus`.
- Link hover requires exactly `Ctrl` on Linux. Explicit OSC 8 destinations take
  precedence over Ghostty's default regex-detected URL/path range. A matching
  result changes the pointer and underlines the visible matching cells; an
  already single-underlined cell becomes double-underlined while hovered. With
  application mouse capture and a shift-capture policy that permits the
  escape, `Shift` first bypasses capture and is removed before modifier
  matching, so the equivalent gesture is `Ctrl+Shift`.
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
- Valid `csi`, `esc`, and `text` binding actions and every accepted paste
  return to the active screen independently of
  `scroll-to-bottom.keystroke`. This preserves Ghostty's action and paste
  behavior when ordinary encoded keystrokes are configured not to scroll.
- Selection is stored in Ghostty's terminal model and formatted by Ghostty for
  clipboard copy. After the public plain formatter applies live trimming, the
  worker scans Unicode codepoints once and searches the finalized ordered
  `clipboard-codepoint-map` newest-first, matching Ghostty's expected-small-map
  strategy and last-overlap rule. It allocates a replacement string only after
  the first match and preserves empty deletion results separately from
  formatter failure. Formatting, destination intent, and optional
  explicit-copy clearing are one `SessionWorker` operation; the resulting
  immutable text and typed intent cross to a queued GUI receiver, so only the
  GUI thread reads or writes `QClipboard`. Explicit action/context copy,
  Linux copy-on-select on left-button release, and select-all share that path,
  with primary-selection fallback resolved from Qt's GUI-thread capability.
  The pane-local `copy_title_to_clipboard` action also writes through that GUI
  adapter, but always targets only the standard clipboard and bypasses every
  selection policy and codepoint mapping. URL, search, and terminal-file
  consumers are likewise unmapped. Libghostty's tracked
  selection-gesture state keeps the drag anchor stable across output,
  scrolling, resize, and automatic selection clearing. Raw binding actions,
  paste, mouse/focus reports, and replayed sequence leaders bypass
  clear-on-typing.
- Terminal-originated clipboard writes stay on libghostty's public normalized
  callback boundary: OSC 52 and OSC 1337 produce the same owned,
  binary-safe request rather than entering a second escape parser. The worker
  applies the live `clipboard-write = ask|allow|deny` policy when consuming the
  sequence; its default is `allow`. `deny` returns a denied callback result and
  omits DA1 feature 52, while `allow` and `ask` accept the request and advertise
  feature 52. Borrowed MIME names and payload bytes are deep-copied into the
  adapter's deferred-effect FIFO before returning to libghostty.

  Only the GUI thread commits the resulting `QMimeData`. All MIME
  representations transfer in one clipboard-ownership transition, including
  embedded NUL bytes; an empty representation remains present and an empty
  representation list clears the destination. Ghostty's standard destination
  maps only to `QClipboard::Clipboard`; both selection and primary map only to
  Qt's Linux `QClipboard::Selection`, with no cross-clipboard fallback.

  `ask` enters the workspace FIFO and exposes at most one correlated dialog at
  a time. The preview is bounded and is never used as the committed data.
  Stable `PaneId` plus `QPointer` validation removes writes from exited,
  removed, or replaced panes before they can commit. The pending workspace
  queue is limited to 64 requests and 64 MiB aggregate; one adapter drain is
  independently limited to 64 requests, 256 representations per request, and
  64 MiB aggregate MIME/payload bytes. A confirmed or denied choice can be
  remembered by changing only that split's runtime policy; the next successful
  config reload reapplies the configured value. Each accepted request
  snapshots whether confirmation was required, so later reloads do not
  reinterpret already queued writes.
  Clipboard reads remain unsupported: public libghostty explicitly ignores
  OSC 52 read requests and exposes no normalized host-read callback.
- `selection-word-chars` crosses the strict config boundary as Ghostty's
  finalized numeric Unicode-scalar vector, including the parser-inserted
  U+0000 boundary. The source UTF-8 string is never reparsed by Qt. The
  value-only runtime snapshot reaches `SessionWorker`, which supplies the
  current vector to the press, drag, and autoscroll-tick events of
  libghostty's selection gesture. Those C events copy the borrowed scalar
  array into event-owned storage before executing beside the terminal. Reload
  therefore affects the next libghostty-classified word press and every later
  word-drag or autoscroll update, including an already active gesture, without
  retroactively changing an installed selection.
- Typed viewport requests cover top, bottom, signed row deltas, absolute rows,
  and the current selection. Select-all and endpoint-adjustment operations run
  as single adapter calls on the session thread. Selection snapshots contain
  untracked Ghostty grid references, so fetch, adjustment, coordinate
  conversion, installation, and endpoint autoscroll are deliberately one
  transaction with no queued boundary between them.
- Selection dragging at the outer physical pixel of the surface arms
  libghostty's pinned autoscroll direction. A precise timer owned by
  `SessionWorker` applies one public gesture tick every 15 ms; each tick scrolls
  one row and resolves the stored physical pointer against the current
  geometry. Interior motion, release, pointer-capture loss, gesture reset,
  terminal reset or destruction, and worker shutdown cancel the timer.
  Capture loss resets only gesture state and preserves the range reached so
  far without invoking copy-on-select.

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

Search, read-only, resize, scrollbar, and bell-border overlays use the same
guarded factory lifecycle.
Replacing or destroying a factory removes all objects it created before a new
factory is attached, and each newly created pane receives the current factories.
The workspace traverses a stable pane snapshot while running QML creation and
destruction callbacks so those callbacks cannot invalidate the split tree being
visited.

The scrollbar consumes the worker's value-only total, offset, and viewport
length after the pane has transactionally accepted them. C++ converts those
`quint64` values to the normalized Qt control state and converts user movement
back to a saturated absolute row, so QML never has to represent a potentially
64-bit row index. `system` uses the current Qt Quick Controls style and appears
only when the active screen has history; `never` hides it. The control is a
per-pane overlay, so live policy reload, split zoom, and track or thumb
interaction do not alter the pane rectangle, terminal grid, or PTY size.

The bell border is likewise a disabled, input-transparent pane overlay rather
than layout decoration. It uses Ghostty GTK's three-pixel half-opacity green
border and 500 ms crossfade. Its opacity derives from the pane latch and the
live `border` feature, so changing configuration can reveal or hide an
already-latched bell without replacing the overlay, pane, renderer, PTY, or
terminal grid. The same derived-state rule controls the title prefix. Reload
does not erase the underlying latch; the next focus, keyboard/IME interaction,
or mouse press still clears it normally.

Resize starts in `TerminalPane`: font metrics and item geometry determine rows,
columns, cell pixels, and surface pixels. The worker resizes both Ghostty's
terminal and the kernel PTY with `TIOCSWINSZ`.

With `scrollback-compression=true`, the worker waits for Ghostty's 250 ms idle
period and then runs libghostty's bounded incremental scrollback compressor,
leaving 1 ms between pending steps. Compression and all other terminal access
remain serialized on the pane's worker thread. Every scheduling path crosses
one live-policy gate, including progressive search and frontend formatting
reads that can restore compressed pages without changing Ghostty's activity
token. A restored-page read that arrives during an incremental traversal
latches one fresh replay after that traversal completes, so a page already
passed by libghostty's verification cursor cannot remain resident. Viewport
movements instead feed the token-based idle scheduler.

Disabling compression stops an armed timer and makes an already-delivered
timeout harmless, but does not decompress existing pages or alter the logical
history allocation. Re-enabling clears the worker's optional cached activity
token before applying the ordinary idle delay, so resident history is
reconsidered even if the terminal has not mutated since the reload.

## Session and environment

Every pane receives the applicable current launch policy, while only the
process's first successfully initialized session receives the one-shot
`initial-command`, positional CLI program, and hold values. A deferred pane
destroyed before start or a failed libghostty-vt initialization does not
consume them; once initialization succeeds, an exec failure does. CLI argv has
highest execution precedence, then `initial-command`, then the ordinary
`command`. Later sessions always skip both one-shot CLI values and
`initial-command`.

The helper transports both configured commands as a tagged raw-byte value.
Shell commands execute exactly as `/bin/sh -c <value>`. Direct commands retain
Ghostty's literal-space-split argv and are passed directly to `execve`; their
bare argv[0] lookup uses the launching process's raw `PATH`, not the later
configured child environment. With no explicit value, Ghostty finalizes its
default shell from `SHELL` or the passwd database; the worker retains
Ghostty's shell-form `sh` fallback for a missing finalized value. Embedded NUL
is rejected at the structured boundary because `execve` cannot represent it.

`abnormal-command-exit-runtime` and `wait-after-command` are live runtime
policies rather than launch snapshots. After a successful child launch, the
worker starts a monotonic clock and watches the Linux process through `pidfd`
readiness when available, retaining the existing child poller as a kernel or
sandbox fallback. At observed exit it freezes the clock before draining or
rendering final PTY output, truncates elapsed time to whole milliseconds, and
samples the newest threshold. A nonzero or signaled Linux exit is abnormal
when that runtime is less than or equal to the configured `u32` value; a quick
successful exit is never abnormal. Child-side lookup and `execve` failures
therefore participate with status `126`/`127`, while parent-side validation or
setup failures that prevent a watchable child do not. The abnormal path remains
visible independently of `wait-after-command` and exposes a bottom Qt failure
banner with an explicit Close action.

A normal exit remains only when the newest `wait-after-command` value is true.
Both a waited normal exit and a non-held abnormal exit close only after
libghostty encodes a pressed key or nonempty IME commit to bytes;
modifier-only input, consumed bindings, and unresolved or dropped sequence
chains remain inert. `--hold` is a distinct, initial-only launch override: it
may present an abnormal result, but terminal input never makes it dismissible.
A new tab retains the exact action-target pane; an empty-context QML request
uses the current tab's recorded active pane. When
`tab-inherit-working-directory` is true, that source's latest nonempty reported
directory takes precedence; when false, or when the source has no
terminal-owned directory, the workspace `working-directory` is retained.
`window-inherit-font-size` independently copies only the source's actual point
size, including manual zoom, or retains configured `font-size` when false. The
four family lists, styles, and metric modifiers always come from the newest
configuration. The tab child starts unadjusted, so a later font-size reload
replaces its inherited visible size.
`window-new-tab-position` is resolved independently against the workspace
selection immediately before creation. `current` inserts after that selected
tab, or appends when no tab is selected; `end` always appends, and either mode
selects the new tab. The action-target pane therefore remains the directory and
font inheritance source without becoming the insertion anchor.

`window-show-tab-bar` is an immediate workspace presentation policy rather than
a surface-creation option. Its exact `always`, `auto`, and `never` values map to
the QML `TabBar`: `always` shows it at every tab count, `auto` hides it for one
tab and shows it at two or more, and `never` hides it. Config reloads and tab
creation/removal both recompute the property, including the one/two-tab
boundary. The surrounding Qt toolbar is a separate control surface and remains
visible, so hiding the tab strip does not remove its new-tab, split, or close
buttons.

Frontend `tabs-location` is an independent Qt presentation policy. Its
`top`/`bottom` value reparents the same retained toolbar and tab strip around
the terminal content, rather than replacing either control or any pane. A
frontend reload applies the placement to every existing workspace and becomes
the default for future windows while preserving the outer window size.

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
to the next entry after the child changes directory. This lookup deliberately
uses the launching process's `PATH`, not a later shared-config `env` override;
the configured `PATH` is nevertheless present in the executed child's
environment. Shell selection is also completed before the override map is
applied, so configured `SHELL` is child payload rather than a selector.

The worker starts from the inherited host environment and injects these
terminal-specific values:

```text
TERM=<finalized Ghostty term; xterm-ghostty by default>
TERMINFO=<resolved private database>
COLORTERM=truecolor
TERM_PROGRAM=ghostty-qt
TERM_PROGRAM_VERSION=<project version>
```

It then overlays Ghostty's finalized ordered `env` map with byte-exact key
deduplication. Keys and values remain raw bytes across the private schema and
launch boundary; ghostty-qt rejects embedded NUL bytes as a safety hardening
because an `execve` environment string cannot carry them losslessly. Finalized
map entries may replace any inherited or injected value, including `TERM`,
`TERMINFO`, `COLORTERM`, `TERM_PROGRAM`, and `TERM_PROGRAM_VERSION`. A concrete
working directory writes its exact logical `PWD` once after those overrides,
even on cwd fallback, so that value wins. Inherit mode performs no write and
therefore retains the inherited `PWD` unless `env` overrides it.

Ghostty resolves the repeatable setting before export. A later
`env = KEY=VALUE` replaces an earlier configured value, `env = KEY=` removes
that key only from the configured map, and a bare `env =` clears the configured
map. Removal and reset are map operations, not inherited-environment unsets:
an absent finalized key keeps its inherited value.

When a child exits, its final PTY output is drained and one last frame is
published. A normal, non-held pane then closes automatically. A held or failed
pane remains visible with a status message.

## Configuration boundary

Configuration is split into two typed domains. Portable terminal behavior,
keybindings, and shared Linux behavior come from Ghostty's standard files.
Qt-owned application behavior comes from the strict
`$XDG_CONFIG_HOME/ghostty-qt/config` frontend file. If `XDG_CONFIG_HOME` is
unset or relative, both domains fall back beneath `$HOME/.config`.

Ghostty's application configuration API is not part of the stable
`libghostty-vt` surface. The Qt executable therefore does not link or retain
handles from that API. Instead, `ghostty-qt-config-helper` links the pinned
`ghostty-internal` shared library. It preserves Ghostty's existing command-line
actions and adds one project-private structured config export. For each load
the Qt-side process adapter invokes, in order:

```text
+validate-config
+show-config-json --ghostty-qt-color-scheme=<light|dark>
+validate-config
+show-config-json --ghostty-qt-color-scheme=<light|dark>
```

The helper runs with the selected `XDG_CONFIG_HOME`, so Ghostty itself owns
standard-file discovery, legacy/preferred-file precedence, include handling,
validation, defaults, and finalization. The private selector installs the
current concrete desktop color scheme before parsing conditional theme state
and is stripped before Ghostty processes ordinary configuration arguments.
The CLI-only `config-default-files` switch is forwarded to every helper
invocation. When false, the helper omits standard candidates before replaying
explicit CLI and recursive `config-file` sources, and the successful snapshot
omits those candidates from the service's watcher set. A setting encountered
inside a config file remains intentionally inert, matching Ghostty's own load
order.
Each JSON document is one exact schema-v1 frontend projection containing all
consumed values plus the finalized current and platform-default binding sets.
The two JSON byte streams must match, preventing a valid A-to-B edit from
publishing a mixed snapshot. The adapter strictly decodes the verified
document into one complete, strongly typed `GhosttyConfigSnapshot`; it never
parses or merges Ghostty's human-oriented `+show-config` output.
Configuration absence remains service state rather than a contradictory flag
inside a snapshot. This keeps the unstable application API and all of its
state outside the long-lived Qt process without reimplementing Ghostty's
configuration grammar.

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
once by the application controller to every live workspace, process
keybindings, and the application lifetime policy, then retained for future
windows. All process consumers continue receiving watched reloads after
resident window retirement.

`FrontendConfigService` independently parses exactly one strict UTF-8 scalar
file. Its current complete schema is `single-instance=false|true|detect` and
`tabs-location=top|bottom`; unknown keys, duplicate keys, malformed
assignments, unsupported values, invalid UTF-8, and partial documents are
errors. A missing file is a successful default generation. The service watches
the file and its nearest existing directory, uses the same 75 ms debounce,
loads watched generations on its own one-thread pool, suppresses stale
generations, and retries failures while retaining its last-good snapshot.
Deleting the file therefore restores the typed frontend defaults.

The application retains the latest successful snapshot from each service and
re-resolves launch options from the immutable process arguments whenever either
one publishes. Precedence is built-in defaults, the finalized shared Ghostty
snapshot, the disjoint frontend snapshot, then explicit CLI overrides. The
`reload_config` application action requests both services. Their reloads remain
independent, so failure in one domain cannot discard the other domain's
last-good generation. The merged result still enters
`ApplicationController::applyLaunchOptions` once per publication, updating all
live workspaces and future windows without cumulative overlay state.

The helper process necessarily has Ghostty action arguments, so the pinned
parser classifies it as a probable CLI launch. An otherwise unset
`working-directory` therefore finalizes to `inherit`; the GTK desktop-launch
heuristic that would choose `home` cannot be reconstructed from the private
helper context. Explicit `inherit`, `home`, tilde, and concrete path values are
still preserved, and the parity ledger keeps this setting and the dependent
tab/split fallbacks partial.

The current typed compatibility slice covers tagged ordinary/initial commands
and live post-exit waiting, ordered initial-input sources, launch and window
geometry, working-directory and inheritance policy, the finalized
child-environment override map, Linux cgroup resource isolation, application
lifetime, typography and terminal appearance, scrollback,
selection/clipboard/mouse/link behavior, resize-overlay presentation, default
and included config-file policy, and the finalized keybinding sets. The README
and machine-checked parity ledger describe the individual keys. One strict
schema-v1 document carries the whole slice, including nullable command
objects, nullable values such as `quit-after-last-window-closed-delay`, the
ordered clipboard replacement list, and both cgroup limits; there is no
version fallback, separate defaults merge, or partially populated snapshot.
Optional cgroup uint64 limits cross JSON as null or canonical decimal strings
so the complete range remains exact despite Qt JSON's double representation.
The finalized `env` map crosses as an ordered array of objects whose `key` and
`value` members are byte arrays, avoiding both UTF-8 conversion and JSON object
key normalization. The decoder rejects duplicate keys, empty values, `=` in a
key, embedded NUL, and every noncanonical shape.
Finalized `input` entries likewise cross as an ordered tagged raw/path array
whose payloads are bytes rather than JSON strings. Each future pane snapshots
that list. Immediately before launch, its worker resolves every path relative
to the frontend process working directory, requires regular readable files,
and reads no more than 10 MiB per source. Resolution completes for the entire
list before pipes, cgroup work, or `fork`; one failure launches nothing.
Successful chunks remain separate and ordered ahead of the worker's `started`
publication, so ordinary GUI input cannot overtake startup input. Reload
changes future panes only and cannot mutate a controller already waiting for
deferred compositor geometry.
Command objects retain an exact `shell`/`direct` tag, byte-array payloads, and
whether the finalized ordinary shell came from Ghostty's default lookup.
Canonical enum tags, optional include markers, working-directory inheritance,
and nullable color alternatives are decoded only at this boundary. The four
font styles remain tagged `automatic`, `disabled`, or
`named` values, and each optional metric modifier remains either null or a
tagged `absolute` pixel/`percentage` multiplier value. Before the fixed-size
palette crosses this boundary, the helper mirrors `termio.DerivedConfig` while
Ghostty's finalized explicit-entry mask is still available. Generation runs
only when `palette-generate` is enabled and that mask is nonempty; it preserves
indices 0–15 and every explicitly assigned extended entry, and uses the
configured background, ANSI indices 1–6, and foreground as the remaining
cube's CIELAB anchors. The grayscale ramp interpolates from configured
background to foreground, and `palette-harmonious` controls light-theme
orientation through Ghostty's pinned `generate256Color` routine. The schema
therefore carries the effective result and cannot carry fewer or more than 256
colors. Appearance then crosses worker threads as a value-only
`TerminalAppearance`: terminal
foreground/background, all 256 effective palette defaults, selection and
candidate/selected search colors, cursor color/style/blink/opacity/text,
bold-color, faint-opacity, and minimum-contrast. Fixed colors and Ghostty's
cell-foreground and cell-background aliases remain distinct until the renderer
has the target cell. Background compositing crosses separately as the
value-only `TerminalBackgroundOptions`, containing the finalized opacity,
explicit-cell policy, and background-image path, multiplier, anchor, fit, and
repeat values. The nullable image path remains an absolute
`GhosttyConfigPath`, including required/optional provenance. Backdrop policy is
GUI-owned: it never enters `SessionWorker` or mutates libghostty terminal
state. `TerminalPaddingOptions` separately preserves the finalized leading and
trailing point values for each axis plus the three-state balance and padding
color policies.

The same schema carries `click-repeat-interval` as Ghostty's finalized
unsigned whole-millisecond value, including the Linux 500 ms default, and
`selection-word-chars` as an array of finalized Unicode scalar values rather
than its source spelling. It also carries the exact finalized
`right-click-action` enum so live reload changes the next worker-resolved
right press without replacing the pane. The strict loader rejects an invalid
interval, unknown right-click tag, and nonnumeric, fractional, surrogate, or
out-of-range boundary members; the parser-provided U+0000 element is preserved
alongside ASCII and non-BMP boundaries. These values are copied through launch
and live runtime options without frontend reinterpretation, then consumed only
on the session thread.

Typography remains entirely on the GUI/render side as a value-only
`TerminalTypography`. It carries one ordered family list and one style
alternative for each of the regular, bold, italic, and bold-italic roles, the
f32-derived point size, the complete ordered feature list, four ordered
variation lists, u21 codepoint-to-family ranges, three synthesis permissions,
the cursor shaping-break flag, five FreeType load booleans, and eleven optional
metric modifiers. Variation values retain their finalized f64 bit pattern in
schema v1, including negative zero and non-finite values, instead of relying on
non-standard JSON numbers.

`terminalFontProgram` takes one GUI-thread font-database snapshot for each
distinct effective font input. A weak process cache shares that immutable
program across panes, DPRs, and metric- or shaping-break-only generations,
while allowing unused entries to expire. Unsupported FreeType
force-autohint/autohint values and light hinting while hinting is disabled are
canonicalized out of the key. Qt font-database changes invalidate the cache
and make every live pane acquire a fresh program without replacing its
terminal or PTY.

The regular metric face defines the grid, and a second shaping-face array
applies OpenType features so a proportional feature cannot silently redefine
terminal geometry. Qt's context font merging consumes each ordered family
list. A native role face is preferred before allowed synthesis; disabled
styled roles use the regular face, while a missing automatic or named styled
role requests synthesis only when permitted. Variation processing consumes
the first matching tag before validity and range checks, matching the pinned
FreeType duplicate behavior. Codepoint-map overlays are compiled in
O(n log n) into sorted disjoint later-entry-wins intervals plus an interned
face table. The renderer performs logarithmic lookup without scanning or
copying the configuration per cell or frame. A mapped regular face must cover
the entire cell grapheme, ignoring only ZWJ and text/emoji variation
selectors.

Dirty rows are converted into maximal compatible text runs by a renderer-free
planner. Font, foreground, text style, selection, invisible cells, defensive
plain `fi`/`fl`/`st` boundaries, and—when configured—the logical cursor split
runs; wide spacer records remain attached to their head cell. `QTextLayout`
shapes each run left-to-right on the physical terminal grid. Fixed-pitch,
single-UTF-16-unit runs can receive one constant letter-spacing correction,
but every text boundary is still checked in device pixels. A mismatch falls
back to exact per-cell layout for that run. This preserves grid correctness
while allowing ligatures and avoiding one layout per ordinary cell.

These mappings do not claim Ghostty's embedded production fallback stack,
FreeType load and synthesis internals, or HarfBuzz positioned-glyph plan:
those contracts are not part of public `libghostty-vt`, so final selection and
shaping remain Qt-owned and the affected entries stay partial. Ghostty's
private generated-box sprite and icon/Nerd Font classification paths also keep
`adjust-box-thickness` and `adjust-icon-height` planned.

The helper receives explicit `--font-family` and `--font-size` arguments before
Ghostty finalizes configuration, rather than overlaying them afterward. This
preserves Ghostty's f32 size and its cloning of an explicit regular family into
otherwise absent styled roles. Explicit working-directory and scrollback CLI
options retain their existing precedence. The exporter and C API overlay are
compiled from a revision shadow in the project cache; the pinned official
Ghostty submodule remains unmodified.

Live reload replaces the complete typography and appearance values on existing
panes without overriding a pane's manual font zoom. A manually zoomed pane
keeps only its local point size while adopting new family, style, and metric
values; resetting the size uses the newest configured default and resumes size
reloads. The pane compares the old and new effective typography—including its
local zoom—before requesting metrics, and the normalized program cache makes
appearance-only, metric-only, shaping-break-only, and transport-only FreeType
changes avoid font discovery and codepoint-map reconstruction.
Directory and font inheritance booleans plus tab
insertion position are workspace-owned creation policy: they affect future
tabs/splits without moving existing tabs or processes or marking an inherited
child as manually zoomed. Tab-strip visibility is workspace-owned presentation
policy and updates immediately without entering a pane or worker. The
maximize and Linux-normalized fullscreen values remain application-owned
creation policy: a successful reload changes every subsequently created
window, including resident and desktop-activation replacements, without
changing any live window's state. The
paired window dimensions are likewise future-window creation policy. Their
cell-to-pixel conversion uses each new window's resolved font size, including
source-pane inheritance, and runs before workspace initialization constructs
its first pane. The current explicit padding is added outside the requested
cell grid and its 10-by-4 minimum. Existing roots retain both their size and
minimum hint on reload. `window-padding-x` and `window-padding-y` are
snapshotted per pane: reload affects later windows, tabs, and splits without
changing existing explicit margins. `window-padding-balance` is live pane
geometry policy: reload preserves the selected grid but republishes its
effective padding and padding-excluded terminal pixel extent. The separately
render-owned `window-padding-color` also updates existing panes without worker
or geometry mutation.

Each pane derives terminal metrics by projecting the regular Qt face to integer
physical pixels at its current device-pixel ratio, applying Ghostty's absolute
signed-pixel or parsed percentage-multiplier alternative, clamping the metrics
that must remain drawable, and converting the result back to logical
coordinates. The pinned sparse modifier map determines application order, so
cell-height-dependent baseline and decoration recentering occurs at that step
rather than through a frontend-imposed fixed sequence. Cursor height retains
the unadjusted cell-height basis, is vertically centered, clamps only to one
physical pixel, and may exceed the cell; cursor thickness starts from one
physical pixel and feeds bar, underline, and hollow strokes. Underline and
strike positions are unsigned and saturate at zero, while overline position is
signed and remains representable when negative. A screen or DPR change
rebuilds these metrics before publishing geometry or repainting.

During one-shot workspace initialization, the authoritative logical workspace
viewport, resolved terminal cell metrics, and selected window's device-pixel
ratio are converted to one complete `TerminalSessionGeometry`. Each snapshotted
padding point value becomes `floor(points * DPR * 96 / 72)` physical pixels on
Linux. Explicit padding is removed before the cell grid is floored; the
retained balance mode then redistributes only the residual surface space.
`false` leaves that remainder at the right and bottom, `equal` centers both
axes, and `true` centers before capping the top from the explicit horizontal
padding and cell width and shifting any excess below the grid.

The resulting value carries the full pane surface, four effective physical
padding values, cell metrics, and rows/columns. One pure conversion drives
scene-graph origin, hit testing, IME placement, mouse encoding, libghostty,
selection gestures, and the PTY. Rows and columns exclude padding, while
`ws_xpixel` and `ws_ypixel` use Ghostty's backend terminal projection and
therefore report the grid's padding-excluded pixel extent.
The same overflow-safe conversion drives later pane resizes, bounds rows and
columns to the PTY range, and rejects nonfinite or not-yet-laid-out viewports.
Only the initial `createNewTab` path moves this value through `TerminalPane`
into the controller-owned pending launch state. `SessionWorker` applies it
before both `libghostty-vt` construction and `forkpty`, so the first frame,
scrollback estimate, and an immediately executing child share the authoritative
startup geometry. Linux exposes PTY terminal pixel extents as 16-bit fields, so
only synthetic grids wider or taller than 65,535 physical pixels saturate at
that final kernel boundary while libghostty retains the overflow-safe `int`
extent.
Ordinary new-tab and split paths have no seed and continue with their own
layout resize. Pane scene attachment and window screen/scale changes also
re-emit the unchanged logical viewport so physical geometry cannot remain at a
stale device scale. A normal window keeps this immediate pre-map path. For an
initially maximized or fullscreen window, workspace initialization still
constructs and registers only the first pane/controller synchronously; no
worker or session thread exists yet. After the one activation-aware
presentation, the pane coalesces hidden and compositor resize state, waits for
a valid exposed viewport to remain stable across two presented-frame
boundaries, and starts exactly one lazily created worker with that newest
geometry. An inactive first tab derives its pending leaf size from the
workspace's current logical split tree rather than retaining hidden normal
geometry. Runtime reload and resize state received while pending are folded
into the launch snapshot. A typed controller-owned
FIFO retains read-only, focus, input, selection, search, and other worker
requests in their original order behind initialization. A shutdown before
exposure cancels the launch and drops that FIFO; destroying the lightweight
controller creates no worker, thread, terminal, PTY, or child. Close policy
therefore sees no process to confirm. Every application-created controller,
including later tabs and splits, shares the same first-session coordinator, so
an immediate later pane can beat a constructed-but-deferred root. The winner
commits its ticket immediately after libghostty-vt creation; a child-launch
error does not restore it. Normal windows, the bare-QML fallback, and every
later tab or split retain immediate scheduling. The workspace-owned
`split-preserve-zoom` navigation
bit also reloads without a pane or worker update and is consulted by each
subsequent successful
`goto_split`; it never changes the current zoom merely because configuration
reloaded. Palette and
fixed cursor defaults are updated through
`libghostty-vt`, which preserves terminal-originated OSC 4/OSC 12 overrides;
OSC 104/OSC 112 reset to the newest configured defaults. Likewise, an active
DECSCUSR cursor style survives a config reload and its reset selects the newest
configured style. Selection, search, cursor aliases/opacity/text, bold-color,
faint-opacity, minimum-contrast, and both background-opacity values are
frontend render policy and therefore update without mutating
terminal-originated state. The base pane rectangle uses the effective terminal
background and Ghostty's rounded `opacity * 255` alpha. A default-background
cell contributes no second fill. An explicit background is opaque while
`background-opacity-cells` is false and uses Ghostty's truncated
`opacity * 255` alpha while it is true. Selection, candidate or selected
search, and inverse presentation always replace that choice with an opaque
cell layer.

Minimum contrast mirrors the pinned Linux cell shader: after bold, inverse,
selection/search, and faint alpha resolve, the renderer first composites the
linear-premultiplied cell layer over the linear-premultiplied pane base. Each
glyph and decoration independently compares against that effective background.
A ratio strictly below the configured threshold becomes opaque white or black,
whichever contrasts more (black on a tie). The calculation intentionally
does not sample content behind the compositor surface. The worker marks the
five pinned terminal-graphics codepoint ranges so only their glyph bypasses
correction; their decorations still participate. Block cursor text overrides
the result, while cursor sprites are corrected after cursor opacity.

The backdrop precedes every grid and padding-extension layer. A configured
background image is fitted against the complete leaf-pane surface, including
explicit and residual padding, so every split has its own origin and placement
rather than sharing one window-wide image. `contain` and `cover` preserve
aspect ratio using the smaller or larger scale respectively, `stretch` fills
both axes, and `none` maps one decoded source pixel to one physical device
pixel. Each of the nine anchors independently chooses the start, midpoint, or
end offset on each axis; both `center` and `center-center` select the middle.
Repeat tiles that fitted result in both directions around the same anchor.
Resizing, moving between DPRs, and changing fit, position, or repeat recompute
only pane-local texture coordinates.

The image multiplier remains Ghostty's unclamped finalized `f32`, including
documented values above one. Composition uses the rounded global background
alpha and the image's straight alpha before producing a premultiplied backdrop.
For an opaque source pixel the final alpha is
`min(background-opacity * background-image-opacity, 1)`; transparent pixels
and uncovered non-repeated regions retain the ordinary global background, and
a zero global opacity suppresses the complete image/background pass. Explicit
cell backgrounds, selection, search, inverse, and padding extension draw over
that pass. Matching Ghostty, minimum contrast deliberately evaluates only the
cell and global configured-background layers and never samples the image.

PNG/JPEG file inspection, decoding, and extraction of opaque straight-RGB and
alpha planes run on a bounded two-thread pool, outside the GUI, render, and
session threads. A process-wide weak cache keys those immutable decoded planes
by finalized path plus file size and modification time. Identical concurrent
requests coalesce, and expired weak entries release unused CPU images. Each
pane still owns its placement, render-thread textures, and composition state.
Generation and cancellation guards discard stale completions after reload or
pane destruction. A changed path keeps the prior image visible until a
successful replacement arrives, an absent path unloads it, and
open/type/decode failure logs while retaining the prior asset. Like pinned
Ghostty, a pane does not reread an unchanged finalized path or retry its failed
source merely because configuration is reloaded; changing away and back
creates a new request. A bounded 250 ms process-wide failure throttle prevents
new panes from stampeding the same failing file identity, while a later new
pane probes current metadata normally. Option-only reload, terminal OSC 11,
global opacity, and image-opacity changes reuse the decoded planes.

The normal RHI path uploads those two opaque planes to a pane-owned public-QSG
material. Linear sampling occurs while RGB and alpha are still straight;
the fragment material then premultiplies, applies Ghostty's image/background
opacity equation, and uses explicit modulo coordinates for repetition before
compositing later cell layers. Separating alpha avoids Qt's ordinary
alpha-bearing texture upload premultiplying before filtering. Qt's software
scene-graph backend cannot execute that material, so its fallback composes the
source-pixel centers on the CPU and presents the premultiplied result through a
simple texture node. That path is deterministic and useful for headless
integration coverage, but scaled varying-alpha edges and repeated tile seams
are source-resolution approximations rather than claims of pixel identity
with Ghostty's shader.

Kitty graphics use libghostty as the sole protocol parser and storage owner.
Direct RGB/RGBA, zlib, PNG, file, temporary-file, and shared-memory
transmission all terminate in Ghostty's decoded image store; the snapshot
bridge accepts every decoded public format, including grayscale and
grayscale-alpha.
The adapter installs one process-wide Qt PNG decoder, aligns Qt's allocation
ceiling with Kitty's 400 MiB decoded-image maximum, and enables every
libghostty medium. `image-storage-limit` defaults to Ghostty's 320,000,000
bytes and applies live to every screen.

The worker deep-copies borrowed decoded pixels into an opaque straight-RGB
plane and an opaque replicated-alpha plane before terminal mutation can
invalidate the handles. Hardware RHI backends filter those planes separately
and premultiply in a custom shader; the software scene graph premultiplies on
the CPU before creating a simple texture node. Physical placement offsets,
source rectangles, and destination sizes are projected through the exact
libghostty cell-pixel geometry, cropped to the visible grid, and hidden during
asynchronous frame/layout geometry disagreement instead of being stretched.
Qt renderer mirrors are additional to Ghostty's configured storage budget;
the two CPU planes and, on hardware, two textures can substantially amplify
memory use for large images. A future packed straight-alpha texture path should
reduce that amplification without weakening interpolation correctness.

Ordinary placements are supported. Unicode virtual placements remain partial:
their definitions are detectable and their U+10EEEE placeholders are kept
blank, but the public library deliberately reports no expanded viewport
fragments. The required renderer-neutral iterator is tracked in
`REQUIRES_UPSTREAM.md`.

`window-padding-color=background` leaves the backdrop visible in every padding
region. Both extension modes copy the nearest resolved cell-background layer
through the retained grid transform: left and right always extend, including
cell alpha. Ordinary `extend` permits top or bottom extension only when the
corresponding edge row contains none of a prompt/continuation semantic,
default-background cell, explicit background equal to the global background,
or pinned perfect-fit Powerline codepoint. `extend-always` bypasses those
vertical guards. Color reload changes only these retained background
primitives; it does not change layout, rows, columns, or PTY state.

Opacity reload is applied to each existing `TerminalPane` and to defaults for
future tabs and splits. It invalidates retained drawing state only where the
changed composition can affect it; it does not replace a pane, scene root,
session worker, terminal, or PTY, and split panes keep independent background
layers. Close confirmation policy and the built-in regex link matcher also
update live; toggling `link-url` never disables OSC 8.
The nullable divider color likewise reloads entirely on the UI thread: a fixed
RGB value paints the exact reserved gaps, while an empty canonical value
restores the opaque frontend fallback in the same nodes without relayout,
focus changes, or terminal-state work.
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

Working-directory, inheritance, and tab-position reloads are workspace-owned
creation policy. They do not replace a running pane's directory, reorder an
existing tab, rebuild rendering, or cross the session thread boundary; they are
consulted only when a later tab or split is constructed. Building split options
from the newest workspace base also prevents a nested split from retaining an
older configured fallback.

One parser/API boundary remains explicit. A null `cursor-style-blink` maps to
Ghostty's initial blinking default, but the public `libghostty-vt` setter takes
only a boolean; it cannot represent the upstream tri-state in which explicit
true/false ignores DEC mode 12. Palette generation has no analogous loss: the
private helper applies Ghostty's exact `termio.DerivedConfig` gate and
generation routine before exporting the effective palette. For a light theme,
`palette-harmonious=false` orients the generated cube and ramp dark-to-light,
while `true` retains the configured light-background-to-dark-foreground
orientation; dark themes are unchanged. Static and conditional light/dark
themes contribute canonical appearance values and explicit palette entries
through the pinned parser. Qt system color scheme changes request a newly
finalized Ghostty snapshot for the selected branch; the serialized
last-known-good service applies it without replacing windows, panes,
terminals, PTYs, or scene roots.

Because the pinned terminal API cannot resize an existing scrollback
allocation, the byte-valued Ghostty limit applies when a pane is created; a
reload affects later panes. `config-file` contributes parser input and watcher
paths rather than a direct renderer setting.

Resize presentation is likewise frontend-only. Each `TerminalPane` owns the
latest accepted terminal grid, a single-shot `QChronoTimer`, and three QML
properties for resize-overlay visibility, text, and pane-relative geometry.
Only a changed column/row pair schedules presentation; a zero-delay GUI turn
coalesces synchronous resize bursts to the latest authoritative grid, and a
subsequent grid restarts the duration. The first accepted grid and further
layout churn during Ghostty's initial 250 ms settling delay remain hidden in
the default `after-first` mode. The deferred maximized/fullscreen path records
its final compositor geometry while the controller is still unstarted, so
pre-worker negotiation is not classified as a resize even in `always` mode.
`never` cancels queued and visible presentation. A mode, position, or duration
reload updates the pane-owned state directly; it does not enqueue worker work,
alter PTY geometry, rebuild rendered rows, or couple split panes. The QML item
is a 120-by-40 logical-pixel disabled rectangle, so Qt scales it for DPR while
it remains input-transparent and local to its leaf pane.

The pinned terminal allocation limit is byte-valued. Ghostty's
`scrollback-limit` therefore passes through exactly. The older
`--scrollback-lines` CLI remains accepted through an explicit estimate of
`max(256, columns * 16)` bytes per requested row, using the initial terminal
width and saturating arithmetic; it is a capacity estimate, not an exact row
guarantee because Ghostty pages also store styles and grapheme metadata.
`scrollback-compression` is independent of that capacity: its exact finalized
Boolean is worker-owned live state, while a reloaded capacity still applies
only to panes created afterward.

## Keybinding compatibility boundary

The config helper exposes a project-private JSON v1 envelope containing
application lifetime, `initial-window`, the unused raw `gtk-single-instance`
compatibility field, the finalized non-empty raw-byte child terminal identity,
the CLI-only default-config-file policy, the finalized ordered tagged
raw-byte initial-input sources,
the finalized ordered raw-byte child-environment override map, the finalized
raw-byte `enquiry-response`, the exact
default-true scrollback-compression Boolean, the exact scrollbar policy, all
five finalized
`bell-features` booleans, the nullable finalized custom-audio path with
required/optional provenance, the raw finite bell volume, the independently
finalized finite precision/discrete mouse-scroll multipliers, the exact
`mouse-hide-while-typing` and `focus-follows-mouse` booleans, the finalized
whole-millisecond `click-repeat-interval`, and the lossless resize-overlay
mode, position, and whole-millisecond duration, the finalized
`scroll-to-bottom` object with its default-true `keystroke` and default-false
`output` fields, the ordered clipboard u21 range and codepoint/text replacement
list, plus Ghostty's finalized
binding sets after
defaults, includes, `clear`, overrides, chains, and `unbind` have been resolved
by the pinned Zig implementation. It
retains full root sequences, named tables, physical/Unicode/catch-all triggers,
canonical action chains, and every binding flag. The C++ parser is strict and
transactional: an unknown schema or malformed dump rejects the reload without
replacing the last-good snapshot.

The production configuration path compiles exactly one immutable
`GhosttyKeybindProgram` for each process configuration generation.
`GhosttyApplicationKeybindings` installs that program in its application-root
matcher, then `ApplicationController` passes the same cheap owning handle to
every `TerminalWorkspace`; each workspace retains it for later pane creation
and passes it to every current `TerminalPane`. The program's shared const
storage contains the node-indexed root and named-table tries, folded triggers,
and compiled action chains, so panes do not repeat parsing or allocate a trie
per surface.

The application root and each pane wrap that common program in a separate
`GhosttyKeybindState`. This mutable matcher is deliberately noncopyable and
nonmovable: its active sequence, queued leader events, and named-table stack
belong to exactly one input surface, and its traversal is correlated with a
worker staging token owned by that surface. A workspace owns only the
immutable program handle, not matcher state. Outside an active sequence,
lookup walks the newest active table
outward and then the root. A one-shot top table is popped as soon as it
supplies a match, including a leader, catch-all, or performable binding.
`GhosttyKeybindStep` reports that stack mutation directly, so `TerminalPane`
can publish its property change without constructing and comparing
before/after active-table name lists. This avoids two `QStringList`
allocations on every keypress. Lookup prioritizes physical identity, then
event Unicode, then the unshifted codepoint, then modifier-specific and bare
catch-all entries at every depth. On Linux/Wayland, native XKB scan codes keep
physical triggers and libghostty's
physical-key encoding layout-independent, while distinguishing top-row/keypad
and left/right modifier locations. Qt does not expose the compositor keymap's
unmodified layout level through `QKeyEvent`, so the fallback unshifted
codepoint remains US-layout-oriented for shifted punctuation. Reading Wayland
keymap state directly is a later input-compatibility slice.

Program availability is independent of its binding count. The default
unavailable program means that no Ghostty binding source was supplied, so a
pane uses the frontend's legacy fallback shortcuts. An available but empty
program means Ghostty deliberately finalized an empty binding set; it therefore
suppresses those fallback shortcuts even though it has no leaves. Collapsing
the two states would silently restore bindings after `keybind = clear`.

Program identity defines a keybinding generation. Installing the same
program handle is a no-op and preserves a matcher's sequence and active-table
stack. Installing any distinct program, even one rebuilt from equal source
values, clears that mutable state; a pane also drops the corresponding staged
session-side leader token before replacement. During live reload,
`ApplicationController` takes an owning generation snapshot after the
application matcher is updated and distributes it application -> workspace ->
panes. Independent monotonic options-update revisions guard the complete
controller, workspace, and pane transactions across synchronous callbacks;
program identity remains responsible only for deciding whether matcher state
must reset. If a callback installs a newer update, the older outer operation
stops instead of overwriting or partially distributing it, even when both
updates deliberately reuse one program. A new window carries its requested
options and program as one pair, then catches up to any configuration that
arrived during factory construction before presentation. Panes created later
inherit the workspace's current handle.

Configuration fanout is also one process input transaction. While the root
matcher and workspaces move to a new generation,
`GhosttyApplicationKeybindings` stores reentrant key events as owning value
snapshots and stores portal/all/global activations as owning compiled action
chains in the same FIFO. A worker-dependent broad action retains that FIFO
until its correlated per-pane barrier completes as well. It drains only after
configuration fanout, any outer root key event, and the current broad action
have finished, so a release cannot race the press's consumed-key bookkeeping,
a later activation cannot overtake terminal state, and a broad action cannot
observe a mixture of old and new panes. Each pane applies the same rule locally
around its runtime snapshot, sequence state, and suspended worker-dependent
action chain.

New-tab and split construction keep a pane detached until its handlers are
wired, then register it as pending before QObject, visual-parent, or QML
overlay publication. Pending panes participate in reload snapshots even though
they are not in the split tree yet. A topology transaction rejects nested
structural actions during publication, and split insertion resolves the source
again by stable IDs after callbacks. Guarded pane snapshots likewise make
overlay creation and destruction safe when QML synchronously reloads config or
destroys an owner.

Direct `TerminalWorkspace` and
`TerminalPane` construction or update APIs that are not supplied a process
program intentionally compile one from their `LaunchOptions`; this keeps
standalone embedding and focused tests self-contained without adding another
production compilation path.

Building a program compiles each canonical action chain once into an owning
positional value. Every entry retains its exact serialized spelling, its
upstream application/surface scope, and an optional executable action.
Unsupported or malformed entries therefore remain inert in their original
position instead of being dropped or accidentally reclassified. The chain
also caches its combined input effect, with closing taking precedence over
`ignore`, and whether every entry is application-scoped. A match copies this
owning chain rather than retaining a node pointer or view, so synchronous
action callbacks may replace the program or reload configuration while the
in-flight event safely retains the matched action-chain snapshot from that
generation. Destruction of the source pane stops guarded dispatch without
leaving a dangling chain. The next event sees the newly installed program.
Reachable-leaf counting and diagnostic serialization share one iterative
depth-first traversal, preserving deterministic root/table and entry order
without putting user-controlled sequence depth on the C++ call stack.

Sequence leader presses are encoded immediately on the session thread and held
as bytes under a generation token. A consumed match drops them; an invalid,
unconsumed, or unavailable performable match flushes the prefix and current key
atomically; `end_key_sequence` flushes only the leaders. This preserves the VT
mode that existed at each leader press and prevents reload or stale queued
operations from leaking input. `GhosttyActionCatalog` compiles each executable
entry into an owning application, pane, or workspace variant. The pane variant
is payload-specific: each alternative contains only the data valid for that
action, such as a scroll amount, table name, search needle, or serialized byte
string, rather than a shared tag plus unrelated optional fields. Workspace
actions use the same typed dispatcher as QML controls. One constexpr descriptor
set owns the simple void workspace mappings, and another owns every
application-scoped Ghostty name alongside its optional Qt implementation.
Frontend clipboard/sequence actions and complex numeric, enum, tuple, and
escaped-string grammars remain explicit parsers. This keeps upstream scope
classification independent from frontend support without repeating action
names across validation and dispatch.

The pane key-event path, application root filter, and all/global workspace
fanout consume that same compiled chain; they do not parse actions or rescan
scope and chain effects per keypress or per destination pane. Raw-string
programmatic entry points compile once at their boundary before joining this
typed route. Dynamic state remains late-bound at execution: cleanup such as
`end_search` still runs when it reports not performed, while clipboard, title,
hover, search, and selection state cannot become stale between preflight and
execution. The complete chain always runs, and actual performance remains
separate from the cached input effect. GUI clipboard reads still distinguish
an absent text MIME representation from an explicitly present empty string,
and an explicit primary-selection paste never inherits the separate
middle-click fallback policy.

Actions whose authoritative input belongs to `SessionWorker` return a
correlated result instead of publishing a GUI side effect independently.
`select_all`, `copy_to_clipboard`, `adjust_selection`,
`scroll_to_selection`, `search_selection`, and the three terminal-file
actions currently use this protocol. A local chain suspends at such an entry,
retains its staged-sequence token and aggregate performed state, and defers
later key or IME input. Accepted request IDs are nonzero and unique among the
pane's in-flight operations; rejected duplicates publish no second result for
the same correlation. The worker reports success, unavailable data, or
failure plus any clipboard/open/search-overlay effect; the pane attempts that
effect on the GUI thread before resuming the next entry. Desktop-opener or
clipboard-service availability does not erase the worker's authoritative
performed state after the terminal operation itself succeeded.
Each completion also carries the pane lifecycle epoch in which it started.
Session exit advances that epoch before UI observers run, resolves pending
callbacks as failed, and prevents an already queued or broad-barrier-buffered
clipboard/open/search effect from publishing afterward. A held pane can start
new actions in the new epoch. Graceful pane shutdown establishes the same
boundary, queues worker teardown before releasing any suspended input chain,
and permanently rejects new worker-backed actions while its close grace period
is still keeping the QObject alive. Deferred key or IME replay is consequently
ordered behind PTY teardown rather than reaching a dying child.
Only after the final entry does the pane apply closing, ignore, performable,
and consumed precedence and replay or suppress the retained key release.
Cancelled pre-start sessions synthesize failed results, while stale results
and destroyed-pane callbacks are ignored by request ID. A completion delivered
through a nested event loop while request dispatch is still unwinding is held
as an early result and consumed by that same continuation frame, rather than
recursively duplicating its sequence token or key-event deferral.
Deferred key snapshots retain their focus epoch, and both pane-local and
process-level drains identify the exact event currently being replayed. New
key or IME input arriving through a nested event loop therefore joins the tail
of the existing FIFO instead of overtaking older snapshots or borrowing a
consumed release from another focus epoch.
The legacy synchronous `executeConfiguredAction()` API still returns an
optimistic `true` when one of these operations is pending; local chains and
the broad coordinator use the internal completion path for the authoritative
performed value.

Process-wide fanout adds an action-major barrier around the same protocol. For
each worker-dependent entry it takes a fresh workspace/tab/tree snapshot,
starts preparation on every target concurrently, and buffers the correlated
results. Once all live targets have resolved, GUI effects commit in snapshot
order before the next chain entry begins. Thus a later clipboard consumer sees
the completed copy, and the last broad clipboard writer is deterministic even
when pane workers finish in another order. A pane or workspace destroyed while
preparation is pending resolves its target as unperformed and cannot redirect
an effect to a replacement pane with the same surrounding topology.
Effect publication itself is resumable: after each committed target it records
the next snapshot index before observing synchronous destruction. Any barrier
which loses a target during preparation or publication resumes on a later
event turn, including destruction delivered while target startup is still
unwinding. Earlier clipboard/open/search effects are therefore never repeated,
and neither a later target effect nor the next chain entry can re-enter a child
while its QObject parent is tearing down. A ready barrier also remains paused
until any process-wide configuration fanout finishes; effects and later
entries can therefore never observe a mixed keybinding/runtime generation.

Destructive lifecycle commitment belongs to the object that owns that
lifecycle, not to the pane action interpreter. `TerminalWorkspace` commits a
close synchronously: it rejects later structural actions and starts every pane
shutdown. It then advances its closed lifecycle through a single queued
publication, so `windowCloseApproved` cannot authorize a destructive host
observer until the originating key event has unwound. Repeated close requests
coalesce, and an application-quit escalation is published only after the
window approval. The pane-originated `close_window` request remains queued as
well, allowing later structural members of that particular chain to finish
before commitment. Process-wide quit confirmation is aggregated and hosted
only by workspaces that remain open; already committed windows participate in
the subsequent shutdown wait without bypassing another window's protection.

Keybinding-originated `open_config`, `reload_config`, and `quit` callbacks
similarly cross the surviving `ApplicationController` queue. `new_window`
enters its existing queued creation path directly, preserving its order
relative to a later quit. The public programmatic `dispatch` API enters routing
synchronously without this keybinding-only wrapper; any resulting close still
uses the owner-delayed approval publication above. A guarded pane resumption is
the defensive fallback for an embedding application that attaches a
destructive direct observer to some other signal; supported lifecycle actions
do not rely on that fallback. Workspace-wide fanout stops safely if a
synchronous pane observer destroys the workspace.
Page actions use the full terminal height; fractional pages multiply in f32
and truncate toward zero, while line and absolute-row parameters retain their
pinned i16 and usize bounds. Non-finite or unsafe fractional values are
rejected instead of reproducing the pinned frontend's float-to-integer crash.
The pane tracks the latest queued resize height so a page action cannot regress
to a stale rendered frame while resize is in flight. Selection availability in
the controller remains an informational GUI cache only. Copy, selection search,
endpoint adjustment, and selection-target scrolling enqueue a correlated
request behind earlier selection mutations and let the worker's current
libghostty state decide performability. This removes both stale-false and
stale-true races across separate input events: a successful action consumes
the key, while an unavailable `performable` action resumes the retained input
through normal VT encoding. Configured select-all is always performed once a
terminal exists, even when the screen is blank, and additionally returns any
copy-on-select clipboard payload through its correlated result so later chain
entries cannot overtake that GUI effect.

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

Plain `write_screen_file`, `write_scrollback_file`, and
`write_selection_file` actions also stay worker-owned through snapshot and
artifact creation. `GhosttyVtAdapter` selects the native full screen, a
derived primary-history range, or the current selection through libghostty's
public terminal formatter with unwrapping enabled and trimming disabled.
Full-screen output
uses the formatter's native complete-PageList range so mixed-width pages under
reflow retain their real endpoints; history derives its last row and column
through libghostty selection adjustments for the same reason. The adapter's
tri-state result distinguishes bytes that are ready—including a valid empty
screen—from absent history or selection and formatter failure. The worker
creates the artifact only for a ready result, fixes and verifies the temporary
directory as owner-only mode `0700`, atomically creates and verifies the
location-specific `.txt` file as mode `0600`, and disables automatic
temporary-directory removal only after every write and permission check
succeeds. This deliberately makes the successful artifact persistent while
every failed or unavailable attempt cleans itself up.

Disposition remains ordered after that same worker snapshot. `paste` enqueues
the encoded absolute path directly in the PTY write FIFO, without entering the
clipboard paste, quoting, newline, bracketed-paste, viewport, or activity
paths; the normal worker-side read-only gate can therefore suppress the bytes
without suppressing file creation. `copy` returns the path for a GUI-thread
standard-clipboard commit, while `open` returns it for the pane's injected
desktop local-file opener. Those effects commit before a local chain resumes
and, for broad actions, only after every target has prepared its result. The
`paste` result is released after its raw path has entered the worker's PTY
write FIFO. A missing history or selection reports unavailable but still
performs the binding without an artifact or GUI/PTY effect, matching the
pinned action's no-data behavior. Receiver-bound request ownership discards a
GUI effect if its pane is destroyed.

Formatting and file I/O currently complete synchronously on the session
worker. That preserves a single ordering point for terminal state and raw
paste, but a very large history or a stalled temporary filesystem can delay
PTY reads and pane teardown. A future bounded, cancellable streaming formatter
would remove that latency without weakening worker ownership.

Read-only mode is a per-pane surface action with ordered UI and worker state.
The pane publishes the new value immediately, refreshes its tab-model role, and
queues the same transition ahead of subsequent input on the session thread.
While enabled, surface-originated keyboard and IME data, encoded mouse reports,
paste, and raw `csi`/`esc`/`text` actions are discarded before reaching the PTY;
the controller also avoids turning those suppressed bytes into speculative
foreground-process activity. PTY output, terminal-generated protocol replies,
including a configured response to each ENQ, resize/focus lifecycle, selection
and copy, search, native scrolling, and other terminal-local actions remain
available. Toggling the mode off resumes later input without replaying anything
discarded while it was enabled.

The visible state mirrors pinned GTK with an input-transparent top-right
`Read-only` badge on the affected pane, so the indicator never captures the
selection or scrolling gestures it describes. Close policy checks read-only
before child-exit, configured confirmation mode, or foreground activity: a
read-only pane therefore requires confirmation directly and when included in
a tab or workspace close. Broad `all`/`global` dispatch toggles each pane from
the same stable process snapshot used by other surface actions.

`GhosttyApplicationKeybindings` performs root app-scoped leaves before the
focused pane lookup, matching Ghostty's app/surface split while leaving leaders
and mixed-scope chains to the pane. A pane that matches `all` or `global`
forwards the chain to the application controller. It executes app actions once and
surface actions over a stable pane snapshot, action-major across the chain;
`unconsumed` and `performable` do not alter broad-binding consumption. Split
container actions resolve from each tab's current active pane during that
fanout, matching the pinned GTK split-tree action boundary. Automatic split
direction is resolved from each originating surface before that placement
redirect. New-tab actions instead retain every pane in the stable fanout
snapshot as their creation source, so activating one new tab cannot redirect
the next action's inherited directory or font size. Placement still resolves
against the selected tab before each creation, matching Ghostty: in `current`
mode the advancing selection produces one contiguous block in stable source
order, while `end` appends that block. Fullscreen, maximize, and window
decoration surface actions are coalesced once per workspace during broad
fanout because every pane maps to the same synchronously toggled Qt host
window; separate workspaces still receive independent transitions. This
intentionally normalizes pinned GTK's per-surface broad toggles, which can
cancel within a multi-pane window.

Each workspace also owns Ghostty's per-window optional decoration override.
Without an override it follows the latest finalized `window-decoration`
configuration. The first action installs `auto` over configured `none`, or
`none` over any decorated preference; the second action clears the override
and reveals whatever configuration is current then. ApplicationController
projects only the effective `none` state to `Qt::FramelessWindowHint` before
first presentation and on later state changes. `QWindow::setFlag` preserves
all unrelated host hints, terminal objects, client geometry, and window state.
Qt's public Wayland API cannot force the configured client/server preference,
so initially decorated `auto`, `client`, and `server` windows all use the
ordinary QPA decoration negotiation. Qt destroys the xdg-decoration object
when an existing Wayland window becomes frameless and does not expose a public
operation to recreate that negotiation. Clearing the hint therefore restores
Qt client-side framing rather than a prior compositor-side frame. Recreating
the native window could recover negotiation, but would add a separate
surface/session handoff problem and is deliberately deferred rather than
hidden inside this incremental action.

Qt Quick exposes only one dominant visibility state, so QML retains whether a
fullscreen window should restore maximized, windowed, minimized, or another
prior visibility state. A maximize toggle during fullscreen changes that
retained state between maximized and windowed without leaving fullscreen,
matching the independent GTK window-state flags. A root first mapped in a
non-windowed state has no compositor-established normal geometry, so QML
restores its retained hidden size on the first transition to windowed; Qt owns
all later normal geometry restoration. In contrast, both title prompt actions
preserve pinned per-surface invocation: every surface in the stable fanout snapshot contributes
one independently captured request, including multiple leaves of the same
split tab. Surface and tab requests share one modal FIFO and are shown without
deduplication.

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
config export without modifying the pinned submodule; shadow creation and the
shared Zig install transaction are lock-protected across build trees. Its Zig
artifacts live in ignored project-local
`.cache/ghostty-internal` and `.cache/zig-global` directories. Only the small
config helper links this private shared library. The installed helper is beside
the main executable and resolves the library from a relative private
`${CMAKE_INSTALL_LIBDIR}/ghostty-qt` directory; disabling the option omits this
configuration path entirely.

The same isolated helper accepts a second project-private, size-bounded JSON
transaction for shell launch preparation. Qt supplies one byte-exact
shell/direct command, the complete inherited and frontend-injected environment,
the finalized integration mode and features, effective cursor blinking, and
the relocatable resource root. The overlay calls pinned
`shell_integration.setupFeatures` and `shell_integration.setup`, then returns
the transformed command, complete environment, and detected shell without
allowing a Ghostty allocation or private handle into the Qt process. The worker
applies finalized user `env` overrides and concrete `PWD` only afterward.
Helper, resource, detection, or setup failure is non-fatal and retains the
original launch.

CMake stages the pinned `src/shell-integration` tree into each build directory,
applies one zero-fuzz downstream patch that changes only the executable used by
the five SSH wrappers, and validates the complete expected resource set. The
submodule remains pristine. Installation places the tree under
`${CMAKE_INSTALL_DATADIR}/ghostty-qt/shell-integration`; runtime lookup is
relative to the executable or the authoritative
`GHOSTTY_QT_SHELL_INTEGRATION_RESOURCES` diagnostic/development override.
Because Qt supplies the executable and override paths as `QString`, those
filesystem paths must be representable by its UTF-8 Unix filename conversion.

The main executable and helper share one constexpr catalog containing every
pinned action spelling and a separate frontend-support decision. This preserves
Ghostty's distinction between a known-but-unsupported action, an invalid
spelling, and a second action without maintaining multiple policy lists. At the
first line of process startup, before argument transcoding or any Qt object,
the frontend classifies raw `argv`. The frontend's documented `--` command delimiter stops
detection before its terminal payload. An exact earlier `-e` also suppresses
delegation to match pinned Ghostty's detector ordering, although `-e` itself
remains unsupported by the frontend launch parser. Standalone frontend
help/version remain separate, deferred actions and the private
`+show-config-json` protocol are rejected, and multiple actions fail closed.
The helper repeats the same allowlist check, while accepting the private export
only in its canonical first-argument form, so a mixed invocation cannot select
an unadvertised internal Ghostty action.

For an accepted public action, Linux `/proc/self/exe` supplies the physical
application path and the compiled helper filename selects its sibling. The
frontend calls `execv` with the untouched argument pointers except for the
helper `argv[0]`; no PATH, working-directory, environment override, shell, or
preflight access check participates. Process replacement preserves PID,
stdin/stdout/stderr, TTY and pager behavior, environment, current directory,
inherited non-close-on-exec descriptors, the process/signal relationship, and
the pinned action's exit status. A missing or unexecutable sibling returns
shell-style 127 or 126 without entering Qt. A configuration-disabled build
retains classification so a supported action gets an immediate feature-boundary
diagnostic instead of becoming a terminal program. Normal GUI launches perform
no helper-path lookup.

The pinned `+ssh` action then spawns and waits for the selected SSH child rather
than replacing the helper. Ghostty owns its wrapper-option boundary, exact
child arguments and streams, TERM/SendEnv options, destination resolution,
built-in terminfo upload and fallback, standard XDG-state cache, and
`128 + signal` child-status mapping. The companion `+ssh-cache` action owns the
same cache's list/query/add/remove/prune/clear grammar and on-disk behavior.
Both remain explicit pre-Qt CLI actions, and the automatically staged Bash,
Elvish, Fish, Nushell, and Zsh functions reach them through
`GHOSTTY_BIN_DIR/ghostty-qt`.

The pinned `+edit-config` implementation then performs its own intentional
second replacement. It loads standard configuration first, creating the
preferred file when both standard files are absent; selects the first non-empty
`VISUAL` then `EDITOR` value; appends the shell-escaped preferred configuration
path; and executes `/bin/sh -c`. The editor value is deliberately shell syntax,
while the selected path is the non-empty preferred file, the non-empty legacy
file, or finally the preferred path. At the pinned revision, a newly created
file remains empty despite the upstream template-writing intent. The resulting
shell or editor status is therefore the CLI status. This is separate from the
GUI `open_config` action, whose Qt desktop-service launch and empty-file
handling have a different contract.

These commands run the pinned Ghostty action implementation in the existing
embedded helper, whose application runtime is deliberately `none`; output
whose finalization depends on GTK runtime state is therefore not described as
byte-identical to the GTK executable. Likewise, upstream action catalogs are
inventories rather than claims that every printed frontend action is already
implemented by ghostty-qt.

The build also runs a small Zig generator against Ghostty's terminfo source and
compiles the result with `tic -x`. The generated database is a dependency of the
application and PTY integration test. A build-tree run finds `share/terminfo`
beside the executable. An installed executable first resolves its private
`${CMAKE_INSTALL_DATADIR}/ghostty-qt/terminfo` directory using only a relative
path, so moving the complete installation prefix does not invalidate it. The
`GHOSTTY_QT_TERMINFO` environment variable is an authoritative diagnostic
override. No system terminfo installation is required. The generated database
always contains Ghostty's `xterm-ghostty` entry; a custom finalized `term`
changes the initially injected child `TERM` but does not synthesize another
entry. The worker inserts that raw value and the resolved private `TERMINFO`
path bytewise, then applies the finalized `env` map, which may replace either
one before `exec`. This preserves non-UTF-8 configured values independently of
the process locale.

Ghostty places generated artifacts in its source-tree `zig-out`, shared by the
developer and release CMake trees. Those presets must not build concurrently.
The staged relocation test installs into a temporary prefix, moves the prefix,
runs the moved main executable through its sibling helper, and runs a Qt
Core-only probe from the moved `bin` directory to verify that it selects the
moved private database. A separate staged-install test verifies the
configuration-specific desktop entry and direct D-Bus service, their distinct
fallback/zero-window commands, exact service identity, actual install-prefix
or configured-absolute executable path, DESTDIR exclusion from embedded paths,
absence of unresolved placeholders, and the config-on/off helper boundary.
Systemd notification integration, icon, AppStream, and distribution packaging
remain separate work; per-pane transient systemd cgroups are implemented.

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
including the four-role typography and exposed physical metric controls, four
search colors, `link-url`, `link-previews`, and all five `bell-features`, is
marked partial or supported.
All four family keys, all four style keys, feature, variation, codepoint-map,
synthesis, shaping-break, and FreeType settings stay partial because final
face and glyph resolution is Qt-owned; font size and the eleven exposed metric
keys are supported. Search actions remain partial because the
public library artifact cannot expose Ghostty's `xev`-dependent search thread,
while custom `link` rules and other upstream keys stay explicitly planned.
All bell features and both audio settings are supported through the public Qt
platform and multimedia APIs; an actual system bell remains dependent on the
Wayland platform/compositor, as Ghostty's system feature is itself conditional
on platform availability. In particular, the pinned Ghostty
`RepeatableLink.parseCLI` still returns
`error.NotImplemented`, so this frontend does not invent a parallel syntax
for user-defined expressions and actions.

## Test boundaries

The default CTest suite has focused layers for each ownership boundary:

- `launch-options` validates defaults, accepted values, invalid CLI input,
  tagged ordinary/initial command projection, live wait policy, typography,
  appearance overlays and bell features, exact f32 CLI font precedence passed
  into helper finalization, scrollback units, mouse-hide policy, and close
  modes.
- `terminal-bell` uses an audio-device-free backend to verify independent
  feature dispatch, playback-time volume clamping, source caching and retry,
  invalid-media recovery, device replacement, and per-pane isolation.
- `terminal-cell-metrics` verifies four-role face selection and regular-face
  grid ownership, ordered feature replacement without grid mutation,
  first-matching variation semantics, later-entry-wins mapped regular faces
  with complete-grapheme coverage, public FreeType approximations, absolute
  and percentage modifiers, physical-pixel rounding/clamping, pinned
  sparse-map order, cell-height recentering, independent centered cursor
  geometry including over-cell height, and DPR projection.
- `terminal-text-runs` tests the renderer-independent maximal-run planner:
  font, color, style, selection, invisible, defensive ligature, and optional
  logical-cursor boundaries; wide spacer handling; interior placeholders;
  edge trimming; and exact fallback-cell coordinates.
- `terminal-geometry` verifies point-to-physical padding conversion, explicit
  padding before grid selection, equal and capped balance modes, projected
  grid origin, padding-aware hit testing, full-surface versus terminal extents,
  excessive-padding safety, DPR behavior, and numeric saturation.
- `terminal-backdrop` exercises all four fit calculations, all nine anchors,
  device-pixel `none` sizing, transparent source pixels, rounded global alpha,
  an image multiplier above one, and zero-opacity composition as pure
  GUI-independent helpers. Software-scene-graph integration can verify the
  deterministic fallback and retained pane lifecycle, but it cannot execute
  the two-plane RHI material. The next target covers backend shader creation,
  linear texture sampling, and modulo seam behavior separately; final Wayland
  compositor presentation remains an interactive check.
- `terminal-backdrop-rhi` verifies that both compiled QSB resources are linked
  into an independent consumer, then attempts OpenGL-RHI checks for
  straight-alpha filtering, hard modulo seams, and texture reuse across
  option-only changes and asset-serial replacement. Those semantic checks skip
  explicitly when Qt's offscreen platform selects its software adaptation, as
  it does in the managed headless sandbox; a green offscreen result alone must
  not be mistaken for RHI pixel validation. A separate XCB plus OpenGL-RHI
  llvmpipe run executes the complete material suite, including
  fractional-DPR seam and relative/global-opacity samples, with seven passes
  and no skips.
- `terminal-rect-batch` verifies that retained hardware geometry grows without
  shrinking, identical and smaller updates allocate nothing, software
  rectangle nodes are pooled and hidden rather than deleted, and switching
  render backends reuses the existing layer objects.
- `ghostty-smoke` exercises terminal parsing/render-state iteration, CJK wide
  cells, key and 1002 mouse-drag encoding, bracketed paste, and terminal query
  callbacks directly through the C API.
- `ghostty-vt-adapter` verifies the application-facing boundary renders value
  snapshots, carries foreground provenance plus explicit/default background
  provenance even for RGB equal to the terminal background, and retains
  effective palette state. It preserves OSC and DECSCUSR overrides across
  appearance reloads, reports
  title/directory/bell effects, handles terminal callbacks, and encodes paste,
  focus, and key input using terminal modes. It also verifies tagged viewport
  scrolling, selection-target alignment, select-all, and endpoint adjustment
  with exact autoscroll, configurable word-boundary scalars on selection
  press and drag, and configurable selection trimming, plus
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
  output draining, exact shell and direct command execution, raw argv
  boundaries, live wait-at-exit sampling and encoded-key dismissal, the
  configured `TERM`, initial private
  `TERMINFO`/`COLORTERM` values, byte-exact finalized environment overrides,
  concrete-directory `PWD` precedence, inherited fallback, and parent-`PATH`
  executable lookup, pinned shell-feature setup before finalized env,
  positional-program forced-mode downgrade, semantic prompt/builtin and
  alternate-screen activity transitions, plus byte-exact terminal-control action writes,
  injected cgroup placement before child exec, soft fallback, hard rejection,
  disabled-policy bypass, reset cache
  synchronization, correlated terminal-action outcomes and effects, process
  exit, explicit-program activity, and an interactive shell's idle/job/idle
  foreground transitions. Read-only cases cover ordered
  toggles, suppressed key/IME/mouse/paste/raw user input, continued output,
  protocol replies, focus bookkeeping, terminal-local work, and clean
  resumption without replay. It also verifies
  coalesced OSC 8 hover queries, stale-coordinate retry signaling, tracked
  targets across viewport hiding/restoration, and independent hover and
  activation leases; regex lookup across UTF-8 graphemes and soft wraps; OSC 8
  precedence; range reflow; viewport hiding/restoration; stable unrelated
  output; and activation invalidation after covered-text replacement. Search
  tests exercise bounded progressive scans, superseding generations, content
  mutation restarts, overlapping ASCII-insensitive matches, navigation, and
  selection-derived needles.
- `terminal-workspace` verifies that active programs request confirmation,
  idle shells follow `true` versus `always`, pending quit resolves on process
  exit, approval is emitted once, and workspace navigation/layout actions
  preserve stable tab and pane identity. Bell coverage verifies per-pane
  latching and interaction clearing, repeated-event attention, independent
  inactive-tab markers, raw-title preservation, split/tab/zoom lifecycle,
  live title/border policy, inactive-window gating, and the disabled real QML
  border overlay. Read-only coverage verifies local and broad per-pane state,
  the active-pane model role and non-hit-testing badge, plus forced
  pane/tab/workspace confirmation for idle and exited children even when
  configured confirmation is disabled. PTY-backed new-tab coverage
  verifies explicit binding sources, empty-context active leaves, broad-fanout
  source stability, local OSC 7/reset fallback, manual font zoom, and
  future-creation policy reloads. Workspace/QML coverage also verifies exact
  live `always`/`auto`/`never` tab-strip visibility, including one/two-tab
  transitions while the surrounding toolbar remains visible. Background-opacity
  coverage uses two live split panes to verify independent effective colors,
  live GUI-only reload, inherited future splits, and stable pane, controller,
  worker, terminal, PTY, and scene identity. Resize-overlay
  coverage verifies all seven pane-relative positions, exact logical size,
  startup and deferred-start suppression, latest-grid coalescing, timer
  restart, grid-preserving pixel-resize rejection, split locality, live mode,
  position, and duration reload, and a disabled real QML item. The focused
  scale-factor-two run additionally checks physical capture dimensions while
  preserving logical geometry. Workspace coverage also verifies coalesced
  fullscreen/maximize routing across panes and windows and prior-visibility
  restoration across fullscreen. It also sends real pointer gestures
  through nested divider gaps to verify exact-split targeting, T-junctions,
  focus preservation, endpoint clamping, cancellation, zoom/tab/scene
  lifecycle, ratio persistence, terminal-cell hit regions, and nullable live
  divider recoloring. Correlated broad-action cases resolve worker results in
  reverse order while asserting snapshot-ordered effects, barrier-delayed
  chain entries and key input, and safe target destruction. A focused second
  CTest run repeats the nested drag and
  color capture at a scale factor of two to keep geometry in logical
  coordinates while checking physical pixels.
- `workspace-foundation` verifies stable tab identity after row removal and
  movement, tab model role updates, and typed action context dispatch.
- `window-ui-controller` verifies configured and live-target rows in one typed
  model, deterministic composite-target ties, case-folded filtering,
  refresh-before-open, selection preservation by command identity, capture
  before modal teardown, and callbacks that may destroy their controller.
- `application-controller` verifies source-less first-pane command/cwd/title
  overrides and initial-session lease resolution, duplicate workspace-local
  pane IDs in different windows, cross-window and cross-tab focus, retained
  hidden quick-terminal presentation, and stale composite-target rejection.
- `ghostty-action-catalog` verifies the supported subset of pinned Ghostty
  action-string parsing—including the five search actions, exact navigation
  grammar, payload-specific owning alternatives, compiled positional chains,
  typed input effects, and direct-surface error categories—and deterministic
  malformed/unsupported results.
- `ghostty-keybind-set` verifies delimiter edge cases, native physical-key
  locations, shifted/unshifted Unicode matching, shared-prefix sequences,
  catch-all priority and recovery, local/broad flags, action chains, named-table
  precedence and one-shot state, independent pane state, and Linux defaults.
  It also verifies compile-on-load scope/effect metadata, inert unsupported
  positional entries, unavailable versus available-empty programs, shared
  immutable storage with noncopyable independent matcher state, same-generation
  preservation, different-generation reset, and owning match snapshots that
  outlive program replacement.
- `ghostty-global-shortcut-portal` verifies XDG trigger conversion, registry
  eligibility and collisions, response-before-reply races, activation routing,
  reload cleanup, and stale callback rejection on a private D-Bus daemon.
- `linux-cgroup` uses the same private-bus harness with a virtual systemd
  manager to verify the exact `StartTransientUnit` signature and properties,
  enablement policy, reply and method failures, `/proc` parsing, and bounded
  membership polling without touching the host user manager.
- `ghostty-config-service` verifies standard paths, typed file/directory and
  optional-include watches, atomic replacement, debounce, queued snapshot
  transport, and retention of the last good value.
- `frontend-config` verifies the independent path fallback, strict UTF-8
  grammar, complete closed key/value schema, transactional rejection, missing
  file defaults, file/directory watches, debounce, non-blocking generation
  handling, and last-good retention. `application-tabs-location` verifies the
  real QML toolbar moves to the configured edge without a binding loop. It
  also checks the pre-window alpha-buffer request, the created window format,
  transparent host clear color, opaque top/bottom chrome, and stable window,
  content item, workspace, and split panes across live reparenting.
- `ghostty-config-export` verifies strict decoding of the complete schema-v1
  frontend projection, including tagged nullable command objects and their raw
  bytes, finalized non-empty byte-valued `term`, ordered tagged raw/path input,
  the ordered raw-byte `env` pairs and their closed validity rules,
  shell-integration mode and exact feature object, the cgroup enum/boolean and
  exact nullable uint64 limits, exact shapes, four role-family lists, tagged
  automatic/disabled/named font styles, nullable tagged absolute/percentage
  metric modifiers, semantic enums, typed nullable fields and include entries,
  canonical colors, clamped background opacity and exact cell-opacity boolean,
  the finalized optional image path and all four image policies, two exact
  padding-point pairs, balance and padding-color enums, the fixed 256-color
  palette, the full unsigned scrollback range, finalized clipboard u21 ranges
  and codepoint/text alternatives, and binding trees.
- `ghostty-config-process-loader` verifies the four-process
  validation/JSON/post-validation/JSON transaction, byte consistency,
  deterministic process failure paths, warning preservation, and real-parser
  `clear`/`unbind` resolution, including canonical byte-string action export
  plus exact trailing font CLI arguments and role finalization,
  ordinary/initial shell and direct command export plus wait finalization,
  default-file suppression and ordered raw/path input finalization,
  default/custom/empty and non-UTF-8 `term` finalization, repeated `env`
  replacement, configured-map removal/reset, include precedence, and raw-byte
  environment transport, default/forced/reset shell-integration policy and
  feature finalization,
  default/custom/empty cgroup policy and full-width limits,
  nullable/capped application-lifetime duration export, and nullable X11
  divider-color canonicalization.
- `shell-integration-resources` checks the staged pinned file inventory and
  every zero-fuzz SSH executable rewrite. `ghostty-shell-integration` verifies
  the strict byte-exact private protocol, limits and invalid inputs, relocatable
  resource discovery, deterministic features with automatic injection
  disabled, and a real forced-shell transformation through the pinned helper.
- `ghostty-config-helper-smoke` runs `+validate-config` through the helper and
  exact pinned Ghostty parser built for the application.
- `ghostty-cli-delegation` verifies the shared raw-argument classifier; same-PID
  process replacement; byte-exact argv, stdin, stdout, and stderr; environment,
  working-directory, and exit-status preservation; missing/unexecutable-helper
  and config-disabled failures; every delegated real pinned action;
  action-option order; pre-Qt operation; direct-helper equivalence; the
  `+edit-config` editor exec, preferred-file creation, path escaping, and
  environment precedence; plus SSH child argv/streams/status, terminfo fallback
  and cache suppression, and isolated SSH-cache lifecycle and file-mode repair.
- `terminal-pane-render` renders frames offscreen, verifies the initial
  placeholder is replaced plus four-role font selection, physical-pixel cell,
  decoration, and cursor metrics at multiple DPRs, selection/cursor/text
  appearance, and the rounded pane versus truncated explicit-cell alpha rules.
  It also verifies compatible-row layout coalescing, device-pixel grid
  validation, old/new logical cursor row invalidation, live cursor-break
  changes without scene-root replacement, and the absence of cursor-only row
  rebuilds when that break is disabled.
  The same probe covers default and explicit-same-RGB backgrounds, opaque
  selection/search/inverse layers, zero opacity, live in-place reload, and
  premultiplied minimum-contrast composition. It also exercises
  sequence consume/replay, performability, viewport/selection action routing,
  release suppression, reload cancellation, correlated worker-action chain
  suspension/cancellation, and tracked OSC 8 hover, copy, and
  release-only activation through a real PTY-backed pane, including live
  output, viewport hiding/restoration, resize-safe masks, and mouse-capture
  modifier transitions. It also verifies typing-hide defaults and live reload,
  terminal-bound text and IME commit eligibility, sequence and asynchronous
  `performable` fallback, stale pointer-epoch suppression, physical-pixel
  motion filtering, pointer/focus/config reveal interactions,
  blank/link/rectangle/base cursor priority, raw DEC arrow and Shift-I-beam
  transitions, live focus-follow reload, inactive
  host gating, same-position/sub-pixel focus suppression, and destructive
  focus-publication observers. The same path covers live
  `link-url` enable/disable,
  byte-exact regex copy, relative-path opening, OSC 8 independence, all three
  link-preview policies, live frontend-only reload, no-query relocation, and
  bounded/escaped display of arbitrary destination bytes. Search rendering
  covers candidate/selected/terminal-selection precedence, resize-safe masks,
  live colors, overlay state, and the distinction between UI and engine-only
  actions.
- `application-lifecycle` starts the complete QML application on Qt's offscreen
  software backend, verifies a short-lived child closes the window cleanly,
  and fails on QML binding-loop diagnostics.
- `application-lifetime-controller` covers immediate, delayed, disabled,
  cancellation, idempotent and changed reload, stale-timeout, transient-window,
  and explicit-quit behavior without wall-clock process orchestration.
- `application-lifetime-resident` closes and retires a real QML root, recreates
  one through the zero-window process action, retires it again, and explicitly
  quits; `application-lifetime-explicit-quit` exercises the real application
  wiring under a disabled last-window policy.
- `single-instance-activation` runs owner arbitration, exact-once acceptance,
  delayed pre-handler replies, mixed bare/standard/GTK action FIFO retention,
  exact `s,av,a{sv}` signatures, typed platform data and string-array variants,
  malformed-action rejection, interface-specific failure acknowledgement,
  owner handoff, release/reacquisition, unavailable-bus fallback, and
  ambiguous timeout behavior against isolated session buses whose sockets and
  runtime directories live under repository-local `./tmp`.
- `application-single-instance` starts two complete offscreen processes on an
  isolated bus, retires the primary's initial QML root to resident zero-window
  state, verifies the bare secondary exits successfully after recreating one
  primary-owned window, then confirms clean retirement and explicit shutdown.
  A second flow starts directly with no QML roots, proves a false secondary is
  an inert successful launch, then uses a true secondary to create exactly one
  first session in the false-started primary. Config-on and config-off builds
  also exercise an explicitly bootstrapped zero-window host plus real cold
  service discovery, activation, teardown, and restart on a private bus. A
  standard-endpoint fixture also verifies that a real fallback executable
  forwards exact launcher token/startup-ID platform data. Separate warm and
  cold flows run the real pre-GUI `+new-window` and
  `+toggle-quick-terminal` clients through `org.gtk.Actions`, including D-Bus
  service activation and primary teardown.
- `ghostty-application-ipc` verifies raw-argv UTF-8 validation, exact
  class/object targeting, caller-cwd insertion, concrete path and tilde
  canonicalization, the pinned home/inherit quirk, opaque `-e` arguments,
  command/title/cwd last-value decoding, exact variant transport, and
  no-fallback bus failure.
- `desktop-activation` verifies exact platform-dictionary filtering,
  pre-application launcher capture, scoped window-show projection, and
  one-shot cleanup. Controller and worker suites additionally cover
  reentrant half-pair destruction and shell-child token scrubbing.
- `desktop-integration-install` stages an installation under repository-local
  `./tmp` and validates configuration-specific desktop/service metadata,
  install-time executable paths, bootstrap arguments, and config-helper
  presence or absence.
- `application-close-dialog` opens and accepts the real QML close confirmation
  around a live child, failing on binding loops or shutdown regressions.
- `ghostty-parity-manifest` checks the pinned revision and upstream-derived
  configuration, keybinding-action, and CLI-action inventories.
- `terminfo-relocatable-install` stages and moves an installation, executes the
  relocated main-to-helper CLI chain plus the helper's private config paths,
  verifies runtime terminfo lookup, and checks valid and invalid explicit
  overrides.

Clipboard and selection-lifecycle tests cover trim policy, copy destinations
and primary fallback, explicit copy-and-clear ordering, automatic selection
commits, select-all, ordered/ranged/overlapping clipboard codepoint
replacements, non-BMP input and output, expansion, deletion, invalid-u21
replacement behavior, live reload, middle-click source/ignore policy,
clear-on-typing key traits, sequence replay exclusions, IME/preedit
transitions, finalized ASCII/NUL/non-BMP word boundaries, double-click
selection, checked timestamp conversion, inclusive consecutive-time and
original-anchor Euclidean-distance repeat thresholds, triple-count clamping,
line and Ctrl semantic-output selection, ignored duplicate Qt double-click
notification, ordinary-release history retention, per-report selection-clear
and gesture-reset behavior, word dragging, and live interval/boundary reload
during a live pane.
Paste-safety tests cover Ghostty's exact bracketed and
non-bracketed policy, live options, control-byte encoding, confirmation-time
mode changes, accepted-only activity and viewport changes, all GUI paste entry
points, immutable worker IDs, multi-pane dialog correlation, queued payloads,
stale responses, session/pane teardown, and preview bounds.
Typed-action tests cover tab and split state transitions. Process-controller
tests cover first-session FIFO lease commit/release, reload snapshots,
close-before-start preservation, immediate-tab and reverse-root start order,
single-start guarantees, immediate child rows/columns/PTY pixels, multiwindow
creation, inheritance, lifetime, and aggregate quit.
The offscreen tests validate QML startup, close/recreate shutdown, dialog
shutdown, scene-graph frame replacement, Qt's requested/created alpha format,
the renderer's emitted alpha values, and pure background-image placement and
composition in a headless environment. The dedicated XCB/OpenGL-RHI run
separately validates the public-QSG background material through llvmpipe,
including straight-alpha filtering and repeat seams. Neither route validates a
production GPU driver's output or the desktop compositor's final composition.
`GHOSTTY_QT_ALLOW_NON_WAYLAND=1` is a test escape hatch rather than a
supported runtime configuration; GPU output and visible translucency must also
be checked interactively in a real Wayland session.

## Deliberate renderer-v1 limits

- Dirty-row value updates keep the thread boundary small for ordinary output,
  and persistent row text nodes restrict `QTextLayout` work to those rows.
  Maximal compatible text runs replace ordinary per-cell layouts, with
  boundary validation and per-cell fallback for unsafe runs. Solid
  presentation and its three cell-derived RHI geometry batches are retained by
  row, so sparse output and cursor-only updates plan and commit only damaged
  rows on the GPU path. The software fallback still flattens cached plans into
  global node pools because per-row scene-node traversal costs more than it
  saves there. A global appearance, geometry, palette, or renderer-backend
  change still rebuilds every visible row by design.
- Text uses Qt's GPU distance-field glyph atlas on hardware RHI backends and
  shapes compatible cells together. It cannot consume Ghostty's private
  selected-face and positioned-glyph plan, so exact fallback, synthesis,
  FreeType flags, and cluster placement remain Qt-owned approximations. There
  is no color-emoji pipeline. Kitty graphics render ordinary placements;
  Unicode virtual placements await an expanded-placement public API.
- `alpha-blending` remains planned. Qt Quick owns text and primitive blending,
  and the current public scene-graph path does not expose an exact mapping for
  Ghostty's `native`, `linear`, and `linear-corrected` modes. The implemented
  background alpha policy is independent of that color-space choice.
- Background images use a dedicated two-plane material on RHI backends so
  straight RGB and alpha are filtered before premultiplication and
  repeated through Ghostty's explicit modulo coordinates. The software
  scene-graph fallback cannot run that material: it precomposes source-pixel
  centers and lets a simple texture node scale or repeat the result. Its
  varying-alpha edges and tile seams are therefore deliberate approximations.
  The normal RHI material is covered through an OpenGL-RHI integration run;
  final production-GPU and Wayland-compositor presentation remains an
  interactive validation boundary.
- Public `libghostty-vt` cannot preserve configured cursor-blink tri-state
  precedence over DEC mode 12, so that case remains explicitly partial in the
  parity ledger. Palette generation does not depend on the text config dump:
  the private structured helper derives the effective palette while Ghostty's
  internal explicit-entry mask is available.
- Configuration beyond the documented typed slice, unsupported keybinding
  actions, user-defined `link` rules, saved sessions, and full production
  packaging remain future work. Source-less desktop activation and
  payload-bearing `+new-window` activation are implemented, including
  activation-token consumption; systemd notification, icon, and AppStream
  layers remain. OSC 8, the
  default `link-url` matcher, link previews, and the incremental search foundation are
  implemented. Search remains partial because the library artifact omits the
  upstream `xev`-dependent thread, mutation restarts its scan, inactive-screen
  results are not retained independently, and the overlay is not draggable.
  Custom `link` parsing remains unavailable in the pinned parser, and OSC
  grouping stays URI-based until the public C API exposes hyperlink identity.
