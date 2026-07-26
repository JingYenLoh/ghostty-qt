# Development and CI

The checked-in CMake presets are the supported developer entry points. Each
preset uses the project-local Zig executable at `.local/bin/zig` and builds in
its own directory. The top-level build requires C++23 for every C++ target and
disables compiler-specific language extensions.

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
cmake --build --preset dev -j"$(nproc)"
ctest --preset dev -j8 --output-on-failure
```

## C++ formatting

Install `clang-format` as a development dependency; version 18 or newer is
recommended. The repository's `.clang-format` captures the existing Qt/C++
style. Format the changed lines in tracked C and C++ files before committing:

```sh
./scripts/check-format.sh --fix
```

Use `clang-format -i <files...>` for new files or intentional whole-file
formatting.

Enable the tracked pre-commit hook once per checkout:

```sh
git config core.hooksPath .githooks
```

The hook checks the exact changed lines in staged file contents, including
partially staged files, without modifying the worktree. Run the same check
directly with `./scripts/check-format.sh --staged`, check unstaged changes with
`./scripts/check-format.sh --diff`, or format all tracked changes since `HEAD`
with `./scripts/check-format.sh --fix`. The fix leaves the index untouched, so
review and stage the result again before committing.

## Wayland decoration qualification

The automated application test uses Qt's offscreen platform and verifies the
requested window flags, state, geometry, and pane/session identity. It cannot
observe xdg-decoration negotiation. Changes to the decoration mapping should
also be checked under a real Wayland compositor with:

```text
window-decoration = server
keybind = ctrl+shift+d=toggle_window_decorations
```

Run `QT_QPA_PLATFORM=wayland ./build/dev/ghostty-qt`, toggle the frame twice in
normal, maximized, and fullscreen states, and verify the terminal sessions
remain intact. The current public-Qt boundary is expected: the initial window
may receive compositor-side decoration, the first toggle is frameless, and the
second uses Qt client-side decoration because the existing native surface
cannot renegotiate its destroyed xdg-decoration object.

The suite includes focused contracts for the libghostty adapter, workspace
identity/action foundation, Ghostty action catalog, dirty-update transport,
typed config/appearance overlays, watched reload, structured keybinding trie
matching, shared immutable keybinding generations, independent pane state,
named tables, all-surface dispatch, portal registration, and replay,
one-shot host-window effects during broad fanout, directional/default-auto
split placement, stable per-tab title overrides and queued title prompts,
typed originating-tab close modes with correlated frozen-target confirmation,
reentrant-model guards, natural-exit pruning, focus preservation, actual
all/global keybinding fanout, and overlapping shutdown grace periods,
exact surface-scoped previous/next window traversal with wrapped
registration-order selection, hidden/closing filtering, destination-pane
focus, reentrant traversal and queued-retirement guards, and per-surface
broad fanout,
exact-source close-surface grammar, adjacent split focus, recursive collapse,
stable correlated read-only/running pane confirmation, atomic resolution,
final-tab escalation, irreversible typed-workspace quit gating,
application/pane-local chain completion, and atomic all/global workspace-close
events, composite process-level window sources, zero-window recreation,
multiwindow lifetime and aggregate quit coordination, standard
`org.freedesktop.Application` owner arbitration, warm/cold service activation,
staged desktop metadata, two-process resident reactivation, exact one-shot
activation platform-data handoff and shell-child scrubbing, reentrant
window/workspace creation teardown,
replaceable per-surface base titles with explicit-empty and OSC-cache coverage,
stable per-surface prompt overrides and a mixed title-prompt FIFO,
per-pane BEL latching, title/border derivation, inactive-tab and native-window
attention, system/custom audio dispatch, interaction clearing, and live
bell-feature/path/volume reload,
terminal appearance and OSC 8/default-regex interaction rendering, the pinned
Oniguruma matcher boundary, and the config-helper process protocol, as well as
raw pre-Qt CLI process replacement, PTY, renderer, application-lifecycle,
parity, exact-parser smoke, and
relocatable-install coverage. List or run individual tests with:

```sh
ctest --preset dev --show-only
ctest --preset dev -j8 -R 'ghostty-vt-adapter|ghostty-link-matcher|terminal-pane-render|launch-options|application-(controller|lifetime)|workspace-foundation|terminal-workspace|ghostty-action-catalog|ghostty-keybind-set|ghostty-global-shortcut-portal|ghostty-config|ghostty-cli-delegation|ghostty-parity-manifest'
```

## ghostty-qt frontend configuration

Qt-owned application settings live in the independent strict UTF-8 file
`$XDG_CONFIG_HOME/ghostty-qt/config`, falling back to
`$HOME/.config/ghostty-qt/config` when `XDG_CONFIG_HOME` is unset or relative.
The parser and `FrontendConfigService` are always built, including when
`GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG=OFF`; that option controls only the shared
Ghostty parser/helper boundary.

The frontend parser accepts the closed `single-instance` and `tabs-location`
schema described in
[Frontend configuration](frontend-configuration.md). It rejects malformed,
unknown, or duplicate assignments transactionally. The service watches the
file and nearest existing directory, debounces changes, reloads off the GUI
thread, and retains the last-good snapshot after a failed reload. A missing
file successfully restores typed defaults.

The application retains the latest successful Ghostty and frontend snapshots
independently. On either publication it rebuilds effective launch options from
built-in defaults, the shared Ghostty snapshot, the disjoint frontend snapshot,
and explicit CLI overrides. The `reload_config` action requests both services.
The `frontend-config` test covers the strict parser and watched-service
lifecycle; `application-tabs-location` covers live QML placement, and the
single-instance integration cases isolate the frontend file explicitly.

## Ghostty configuration parser

Configuration is enabled by default with
`GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG=ON`. Because the exact Ghostty application
parser is outside `libghostty-vt`, the build produces a private
`ghostty-internal` shared library and links it only into
`ghostty-qt-config-helper`. Each load is a four-process transaction: validate,
request the private schema-v1 `+show-config-json` projection, validate again,
and request the same projection again. The two JSON byte streams must match,
so each document carries one finalized current configuration together with its
platform-default keybinding baseline. The loader does not parse or merge the
human-oriented `+show-config` output.

The same private executable is the transparent process-replacement target for
`+edit-config`, `+explain-config`, `+help`, `+list-actions`, `+list-colors`,
`+list-keybinds`, `+show-config`, `+ssh`, `+ssh-cache`, and
`+validate-config`. One shared catalog records every pinned action spelling
and its explicit frontend support decision, so known-but-unsupported actions
remain distinguishable from invalid spellings without maintaining parallel
allowlists.
The frontend classifies raw arguments and uses Linux `execv` before QString
conversion or Qt initialization; it does not reuse the buffered, timeout-bound
`QProcess` configuration protocol. Therefore public CLI streams, TTY/pager
state, environment, PID, process/signal relationship, and exit status remain
caller-owned. The helper's embedded application runtime is `none`, so
runtime-specific config finalization can differ from GTK even though the action
implementation is the exact pinned code.

`+ssh` remains Ghostty's wrapper: the helper spawns and waits for the selected
SSH child, prepends the pinned TERM/SendEnv options, optionally installs the
built-in terminfo payload, and maps a child signal to `128 + signal`.
`+ssh-cache` operates on Ghostty's standard
`${XDG_STATE_HOME}/ghostty/ssh_cache`; tests must isolate `XDG_STATE_HOME`,
`HOME`, and `TMPDIR` beneath repository-local `./tmp`. Explicit action support
does not imply the separately tracked shell-script injection that wraps an
ordinary `ssh` command automatically.

`+edit-config` continues through the pinned helper into `/bin/sh -c` using the
first non-empty `VISUAL` or `EDITOR` value and a shell-escaped standard config
path. Tests must always remove inherited editor variables or install a
deterministic fake editor; invoking this action with a developer's environment
can intentionally replace the test process with their interactive editor.

The schema-v1 projection includes the finalized working-directory,
split/tab/window directory inheritance policies, new-window/tab font-size policy,
and the canonical `current`/`end` new-tab position plus the
`always`/`auto`/`never` tab-bar visibility policy, complete canonical 256-entry
palette, the canonical `navigation`/`no-navigation` split-preserve-zoom policy,
four ordered regular/bold/italic/bold-italic family lists and their tagged
automatic/disabled/named styles, the f32 font size, eleven nullable tagged
absolute-pixel/percentage metric modifiers, selection colors, cursor
color/style/blink/opacity/text, bold-color, faint-opacity, the nullable
frontend-only unfocused-split fill, finalized unfocused-split opacity,
split-divider color, the boolean `link-url` setting plus the three-state
`link-previews` policy, the exact `system`/`never` scrollbar policy, the five
finalized `bell-features` booleans, nullable finalized bell-audio path and its
required/optional provenance, raw finite bell-audio volume, independently
finalized finite precision/discrete mouse-scroll multipliers, the exact
`mouse-hide-while-typing` and `focus-follows-mouse` booleans, the finalized
whole-millisecond `click-repeat-interval` including the Linux 500 ms default,
`selection-word-chars` numeric Unicode-scalar array including Ghostty's
mandatory U+0000 boundary, the raw false/true/detect
`gtk-single-instance` mode as an unused schema-v1
compatibility field, the boolean `initial-window` startup decision, and the exact
boolean/nullable-millisecond application lifetime policy. The
export and process-loader tests verify exact wire validation, typed semantic
values, nullable alternatives, Unicode scalar range/surrogate rejection,
transaction consistency, and default-aware keybinding diagnostics;
launch-option and process-loader tests verify that
explicit font CLI arguments enter both structured queries before Ghostty
finalization, preserving f32 and styled-role defaults while the public
`+validate-config` action retains its exact action-specific grammar.
Terminal-cell-metric and
pane tests verify four-role selection, physical-pixel/DPR projection,
decoration and cursor geometry, live reload, and manual zoom. Adapter tests
verify that
config-default changes preserve OSC/DECSCUSR terminal overrides; and
`terminal-pane-render` verifies
frontend-only terminal color/style, retained split dimming, live link-matcher
rules, and live preview policy. Workspace render tests cover dimming's actual
focus and search predicate, configured-background fallback, live reload,
split/tab/zoom/window/scene lifecycle, divider recoloring, unset restoration,
handle lifecycle, and logical-to-physical scaling at 1× and 2×.
This division keeps parser, terminal state, and renderer responsibilities
independently testable.

The configuration exporter and its C API overlay are compiled from the
project-local revision shadow. They do not modify or create commits in the
official pinned Ghostty submodule; a change that truly requires an upstream
public API remains documented in `REQUIRES_UPSTREAM.md`.

The additional Zig outputs and global package/artifact cache live under:

```text
.cache/ghostty-internal/<ghostty-revision>/
.cache/ghostty-link-matcher/<ghostty-revision>/<optimization>/
.cache/zig-global/
```

Both paths are project-local and ignored by Git. Revision-shadow creation and
the config-parser install transaction are serialized across presets. The main
Ghostty VT build still shares source-tree outputs, so presets must not build concurrently. A
cold config-parser build is much larger and slower than an ordinary incremental C++ rebuild. To
work on the application without this integration, configure manually with
`-DGHOSTTY_QT_ENABLE_GHOSTTY_CONFIG=OFF`; that is a development option, not the
normal feature set.

The matcher cache is independent of the private configuration library.
`libghostty-vt` intentionally disables Oniguruma, so CMake builds the small
`zig/link_matcher` C ABI from Ghostty's pinned `src/config/url.zig` and vendored
Oniguruma package. `ghostty-link-matcher` tests the C++ byte-range boundary;
`ghostty-link-matcher-upstream-corpus` compiles and executes Ghostty's complete
URL/path corpus against that same engine. Both searches and the application use
Ghostty's 100,000-step retry budget.

The five focused config tests have distinct boundaries:

- `ghostty-config-service` exercises filesystem discovery/watch/debounce,
  missing optional includes, generation-safe asynchronous reload, and last-good
  snapshot behavior with an injected loader.
- `ghostty-config-export` exercises the strict schema-v1 decoder independently
  of process execution, including exact typography role lists, tagged style and
  metric alternatives, fields and types, the full unsigned scrollback range,
  nullable values, and malformed keybinding trees.
- `ghostty-config-process-loader` uses a fake helper to make protocol ordering,
  post-query validation, byte-for-byte consistency, warnings, timeouts,
  crashes, and failures deterministic; real-helper cases verify finalized
  surface-inheritance booleans, exact font CLI forwarding and role
  finalization, Ghostty's effective
  `clear`/`unbind` result, and structured sequences, chains, catch-all triggers,
  flags, and named-table transport.
- `ghostty-config-helper-smoke` runs the actual helper against the exact pinned
  parser with an isolated `XDG_CONFIG_HOME`.
- `ghostty-cli-delegation` combines allocation-free classifier cases with a
  byte-framed fake helper, every delegated real action, invalid and reordered
  action options, the pinned editor exec and selection contract, deterministic
  SSH argument/stream/exit/signal and terminfo/cache phases, isolated SSH-cache
  lifecycle and mode repair, missing/unexecutable-helper failure, the
  config-off boundary, and moved-prefix main-to-helper discovery. Every fixture
  lives under the repository-local `./tmp` or build tree.

The app lifecycle test also uses an isolated config home, so it never reads a
developer's real Ghostty configuration.

`ghostty-keybind-set` covers finalized root and named-table tries, including
Linux native physical locations, shifted punctuation lookup, sequences,
catch-all fallback, table precedence/one-shot activation, direct stack-change
reporting without active-table list snapshots, action chains, and exact local
`unconsumed`/`performable` behavior. It also checks compile-on-load
owning chains, exact positional spellings, cached scope/effect/application-only
metadata, inert unsupported entries, and match lifetime across program
replacement. Shared-program coverage verifies one immutable program generation
can back independent noncopyable matcher states, that sequence and table state
remain surface-local, and that an old program remains usable after another
matcher changes generation. It also distinguishes the unavailable fallback
sentinel from an available empty Ghostty binding set, verifies that reinstalling
the same program generation preserves mutable state, and verifies that a
different generation resets it.
The same executable includes a pathological 32,768-key sequence to ensure
binding counting and serialization remain iterative rather than consuming one
C++ stack frame per configured key.

Session and pane tests cover payload-specific typed execution without
keypress-time reparsing, byte staging, invalid-sequence replay, table reset,
reload cancellation, full/fractional/line/absolute viewport movement,
selection-target scrolling, default and configured discrete wheel movement,
precision pixel scrolling, fractional/reversing accumulation, identical local
and DEC-captured row counts, worker-rechecked fractional-capture selection
clearing, bounded extreme dispatch with retained debt, live multiplier reload,
typing-hide defaults and reload, terminal-bound key and IME eligibility,
invalid-sequence and asynchronous `performable` fallback, stale
pointer-activity-epoch suppression, physical-pixel movement filtering, reveal
interactions, blank/hyperlink/default cursor priority, focus-follow defaults
and live reload, inactive-window gating, same-position/sub-pixel focus
suppression, and destructive focus-publication observers, finalized default
and custom ASCII/NUL/non-BMP word boundaries, worker-owned timestamp,
interval, and distance repeat classification with checked Qt timestamp
conversion, inclusive consecutive-time and original-anchor Euclidean-distance
thresholds, triple-count clamping, single/word/line and Ctrl semantic-output
behavior, duplicate Qt double-click suppression, ordinary-release history
retention, distinct button/motion/wheel reporting side effects, word-drag
behavior, live interval/boundary reload during an active pane, select-all,
endpoint adjustment/autoscroll, worker-authoritative
selection-dependent performability across stale-false and stale-true GUI
cache windows, exact empty/nonempty selection-search effects, byte-exact
CSI/ESC/text actions,
full-reset cache synchronization, long OSC 8 URI extraction across viewport
and alternate-screen state, tracked output/reflow/scroll/pruning behavior,
latest-request coalescing, stale-result rejection, stable live-output hover,
logical-line UTF-8 mapping, default-regex matching across graphemes and soft
wraps, OSC 8 precedence, live `link-url` reload, byte-exact copy, relative-path
opening, range mutation invalidation, release-only tracked activation, exact
`link-previews` policy, frontend-only reload without replacement queries while
the pointer remains on the link, occupied-guard release back to physical hit
testing, left/right overlay relocation, and bounded/escaped destination
presentation.

Workspace tests cover application-action precedence, inactive-surface fanout,
typed all/global chain reuse without per-pane parsing,
application -> workspace -> pane shared-program distribution, current and
future panes sharing one program while retaining independent matcher state,
same-generation preservation, different-generation sequence/table cleanup,
and direct pane/workspace fallback compilation. Reentrant update coverage
installs newer options from synchronous window-factory, workspace, pane, and
sequence-staging callbacks and verifies that the older outer transaction
stops. Initialization observers may supersede configuration or destroy their
workspace without stale geometry or lifetime access, while owning matched
action-chain snapshots remain valid across replacement. Source destruction
safely stops guarded dispatch. Available-empty coverage verifies that an empty
Ghostty program remains distinct from the unavailable legacy-fallback state.
These contracts remove per-pane trie/action recompilation: keybinding
compilation and immutable trie/action storage scale with configuration
generations, while runtime-option propagation remains proportional to the
number of open panes. Cross-window tests inject a complete global key event
while a later workspace still has the old generation and verify FIFO replay
only after process-wide fanout. Nested-reload coverage also verifies that a
root release waits for its outer press bookkeeping. Pane-publication tests
reload from QML overlay completion during both new-tab and split construction,
and destruction tests cover model insertion, tab-title publication, and
tab-bar visibility callbacks. Workspace tests also cover
owner-delayed lifecycle publication, final-surface and final-tab action-chain
completion, synchronous host destruction after local/all/global key events,
direct all-pane fanout unwinding under both normal close and unexpected
observer destruction, ordered application-quit escalation, protected
multiwindow confirmation re-hosting, and deferred open/reload callbacks that
destroy their originating workspace. They also cover stable tab
insertion/reordering/index selection, wrapped split
traversal, mutable and equalized layouts, future-only split working-directory
policy reloads with explicit and nested sources, source-stable new-tab
directory/font inheritance across explicit, QML-style, reset, reload, and
broad-fanout paths, exact
`current`/`end` new-tab placement across live reload and broad fanout, live
`always`/`auto`/`never` tab-strip visibility across one/two-tab transitions
without hiding the surrounding toolbar, and split zoom lifecycle. Zoom coverage
also verifies the exact `navigation`/`no-navigation` config values, live policy
reload, transfer on successful spatial and tree-order `goto_split`, inert failed
navigation, and unchanged direct-activation and structural unzooming.
Direct tab-title coverage verifies Ghostty's canonical byte-string escape
grammar and UTF-8 validation, inactive and broad-action source-tab targeting by
stable identity, empty payload reset, effective/raw model-role notifications,
and preservation across pane focus, OSC title updates, tab insertion, and
reordering. Surface and tab prompt coverage verifies exact void grammars,
raw override/base versus current-display snapshots (including modeled tab
zoom), exclusion of tab/fallback text from surface prompts, focused
caret-at-end editing without select-all, exact OK/blank/Cancel behavior,
masked live base updates, stable targets across tab movement and pane removal,
target deletion and stale-ID safety, and one mixed FIFO with one request per
broad-target surface and no split-tab deduplication. Title-copy coverage checks
strict grammar and finalized helper transport, local performability, absent
and explicit-empty no-ops, exact Unicode/whitespace, override/base precedence,
tab/fallback exclusion, standard-versus-primary destination, and stable broad
Qt last-writer behavior without focus or selection changes. Application tests
exercise both headings and exact Unicode text through the shared real QML
dialog and verify focus restoration. Process-controller tests cover duplicate
PaneIds in separate windows, live and stale source inheritance, one-shot
command removal including the first lazy surface after suppressed startup,
live/replacement config, delayed-exit cancellation and factory failure, clean
zero-window residence, application-wide confirmation and shutdown,
cancellation, dialog re-hosting, ownership-contract rejection, and synchronous
destruction during presentation and creation observers. Private session-D-Bus
integration also covers a false launcher that leaves a false-started primary
at zero windows,
followed by a true launcher that creates exactly one primary-owned surface.
With shared Ghostty configuration enabled, the same suite forces the exact
service bootstrap flags over a contradictory frontend file; the config-off
build proves that activation does not depend on the parser helper. Both call
the standard endpoint against a warm zero-window process and let the bus
discover, start, activate, retire, and restart a cold service using only its
starter-bus environment. Focused activation tests also verify exact D-Bus string filtering,
FIFO platform-data retention, pre-Qt launcher capture, scoped window-show
projection, cached race-free worker snapshots, fallback-executable forwarding,
cleanup, and removal from terminal child environments. The DESTDIR-staged
desktop integration test checks
configuration-specific IDs, relative or absolute final executable paths,
distinct desktop fallback/service-host arguments, and config-helper presence.
BEL coverage keeps the worker event and GUI presentation boundaries separate:
pane cases verify transition-only latch notification versus every-event bell
publication, active title and border derivation, live feature changes without
state loss, and clearing on focus, non-modifier key/IME interaction, or any
mouse press. Workspace cases verify that only the active surface decorates the
raw effective title, inactive split latches remain independent, inactive tabs
publish and clear their stable attention role, and repeated eligible bells
request native host attention only while the window is inactive. Real QML
coverage checks the full-pane, non-hit-testing border overlay without changing
pane, terminal-grid, or PTY geometry. Config export, process-loader, and launch
option cases cover the strict five-boolean feature object, nullable finalized
path with required/optional provenance, raw finite volume, and defaults.
Device-injected tests require no audio hardware while proving every-event
system/audio dispatch, path-cache reuse and replacement, playback-time volume
clamping, missing-source retry, invalid-media recovery, independent per-pane
players, live next-BEL reload, and safe completion after a destructive bell
observer. Automated tests do not claim that an offscreen environment emitted
sound; real Wayland system-bell and media-backend playback remain manual smoke
boundaries.
Per-pane read-only coverage exercises local and stable broad toggles, active-pane
model and input-transparent badge publication, suppressed keyboard/IME/mouse,
paste, and raw-action writes, live terminal replies and focus bookkeeping,
clean input resumption, and unconditional pane/tab/workspace close confirmation
for idle and exited children. Plain file-action coverage checks the exact
three-location and three-disposition grammar, shorthand versus explicit
`,plain`, rejection of malformed parameters, and the recognized-but-unsupported
VT/HTML formats. Adapter cases verify full active-page, primary-history, and
exact selection ranges, including alternate-screen behavior, soft-wrap
unwrapping, preserved trailing spaces, trailing blank-row removal, and the
ready-empty versus unavailable distinction. Worker and pane cases verify exact
basenames and bytes, persistent owner-only artifacts, copy and desktop-open
effects, raw FIFO-ordered path writes without paste framing, read-only
suppression after creation, no-data effects, correlated success/unavailable/
failure completion, and receiver teardown safety. Pane chains verify exact-ID
matching, correlated select-all copy-on-select, clipboard/open commitment
before continuation, duplicate-ID rejection, nested-loop early completion,
worker-performed state across a rejected GUI opener, replay-reentrant key/IME
FIFO order, focus-epoch handling, destruction during finalization, and
cancellation before deferred session startup. Lifecycle cases additionally
hold both queued and delayed search/file effects across session exit or
graceful shutdown, verify that no stale GUI effect is published, and verify
that suspended chains still resolve their input barriers. Broad coverage
starts every pane before waiting, resolves workers in reverse order, commits
effects in stable snapshot order, holds later actions and input behind the
barrier, preserves replay-reentrant process input order, waits for
configuration fanout, and treats synchronously or asynchronously destroyed
targets as resolved without redirecting their effects. Selection-specific
cases cover action-major adjust/scroll/search/copy barriers and resumable
search-effect publication when an observer destroys a workspace, without
duplicating earlier effects or advancing during teardown. A buffered broad
search effect also expires if its originating pane exits before the remaining
targets resolve. Tests that inspect
persistent artifacts remove their temporary directories explicitly.
Other adapter and worker tests
also cover local/remote OSC 7 filtering, encoded and raw paths, and stale launch
directory fallback. They additionally verify same-batch valid/invalid OSC 7
ordering, inherited logical `PWD`, exact symlink-sensitive concrete paths, and
child-directory lookup for relative `PATH` entries. PATH tests also cover the
pinned empty-entry and unset defaults plus exec-time candidate fallback. Worker
tests also verify Ghostty's stale requested `PWD` and non-fatal `chdir` behavior
for missing and existing non-directory launch paths.
`ghostty-global-shortcut-portal`
uses pure registry tests plus a private
D-Bus daemon to exercise response races, reload, cleanup, and activation.
The same boundary does not expose the post-derivation palette and explicit-entry
mask that `termio.DerivedConfig` needs for exact
`palette-generate`/`palette-harmonious` behavior. Also,
the public terminal option for cursor blink is boolean rather than Ghostty's
configuration tri-state. The parity ledger keeps those limitations planned and
partial, respectively.

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
manifest and CMake reference `GHOSTTY_REVISION` and that the checkout matches
it, then derives fresh inventories from the upstream Zig declarations and
reports deterministic drift. CTest runs the same contract as
`ghostty-parity-manifest`. Update `GHOSTTY_REVISION`, the submodule gitlink,
the extractor (if upstream declarations changed shape), and the manifest
inventory in one reviewed change when intentionally advancing the snapshot.

For a sanitizer run, install Clang and use:

```sh
cmake --preset sanitize
cmake --build --preset sanitize --target clean -j"$(nproc)"
cmake --build --preset sanitize -j"$(nproc)"
ctest --preset sanitize -j8 --output-on-failure
```

LeakSanitizer itself requires ptrace support. In a restricted container that
denies ptrace, keep ASan/UBSan enabled but disable only leak detection and avoid
the test preset's overriding environment:

```sh
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/sanitize -j8 --output-on-failure
```

The sanitizer preset instruments the project's C and C++ targets with ASan and
UBSan, enables frame pointers, and stops on undefined behavior. It also changes
the pinned Ghostty and link-matcher builds from `ReleaseFast` to `ReleaseSafe`,
retaining Zig's safety checks. The `libghostty-vt` archive, the Zig matcher,
and its C Oniguruma archive are not Clang ASan/UBSan-instrumented. Failures can
still be visible at an instrumented C++ API boundary, but this preset does not
provide complete sanitizer coverage inside those archives. The private
config-parser build remains `ReleaseFast` and is also not
sanitizer-instrumented; its process boundary is intentional containment, not a
replacement for upstream testing.

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
