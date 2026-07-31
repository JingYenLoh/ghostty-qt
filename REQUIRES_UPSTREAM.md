# Features requiring upstream Ghostty changes

ghostty-qt consumes Ghostty only through the official `ghostty-org/ghostty`
submodule and its public APIs. Do not carry local Ghostty source patches in this
repository. When a parity item needs an upstream change, describe the missing
public contract here and do not mark the unavailable behavior supported. Use
`partial`, `planned`, or `blocked_upstream` as appropriate until an official
Ghostty commit provides it.

## ENQ response payload length

**Status:** finalized binary `enquiry-response` values, live reload, read-only
protocol replies, and exact 1–255-byte responses are implemented; longer
responses are blocked by the pinned public libghostty bridge.

Full Ghostty's termio stream handler places the configured byte slice directly
in one PTY-write request for every ENQ (`0x05`) and does not impose a
255-byte configured-length limit. The public `libghostty-vt` callback contract
also returns a pointer and explicit length, and its PTY-write callback accepts
a pointer and explicit length. Its internal standalone stream bridge
nevertheless copies the response into a fixed 256-byte sentinel buffer and
returns without writing when `response.len >= 256`.

The public header also says the returned memory need remain valid only until
the enquiry callback returns, although the bridge can read that returned
pointer only after the callback has returned. ghostty-qt avoids the ambiguity
by keeping adapter-owned response storage stable across the complete
synchronous VT-write operation.

Consequently, ghostty-qt preserves the finalized raw bytes, including embedded
NUL, and responds byte-exactly once per ENQ for lengths 1 through 255. The
empty default is silent. Values of 256 bytes or more reach the public callback
but the pinned bridge silently suppresses the reply. This was verified against
official upstream commit `c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3` on
2026-07-28.

### Why ghostty-qt does not patch or duplicate the bridge

A local Ghostty source patch would violate this repository's
official-submodule policy. Parsing ENQ independently in `SessionWorker` would
create a second streaming terminal parser and could double-reply for shorter
values while diverging on fragmented input, cancellation, and future parser
changes. Rejecting longer configuration values would also differ from
Ghostty's parser and full termio behavior.

The frontend therefore keeps the public callback installed for every finalized
value and records the public bridge's length limitation explicitly.

### Required upstream contract

Official `libghostty-vt` should forward the enquiry callback's complete
explicit-length result to the existing PTY-write callback without a fixed
255-byte payload ceiling. The implementation may change its internal handler
slice type, allocate temporary sentinel storage, or use another ABI-neutral
mechanism; no public signature needs to change.

The path must:

- treat a zero-length response as silent;
- preserve every byte, including embedded NUL;
- emit exactly one ordered PTY write for each parsed ENQ with a non-empty
  response;
- support fragmented input and multiple ENQs in one VT-write call;
- clarify the returned-buffer lifetime and avoid retaining it beyond the
  enclosing synchronous VT-write operation; and
- avoid imposing a smaller response limit than full Ghostty's termio path.

### Upstream acceptance evidence

Public C API tests should verify byte-exact responses of lengths 1, 255, 256,
and a substantially larger value, plus an embedded-NUL value. They should also
cover the empty response, repeated and fragmented ENQs, one write per ENQ, and
callback-result storage released after the documented synchronous lifetime.

### ghostty-qt follow-up after upstream support lands

Once the fixed buffer is removed in an official, publicly reachable Ghostty
commit:

1. Update `GHOSTTY_REVISION` and the official submodule gitlink together.
2. Extend adapter and worker integration tests above the old 255-byte boundary.
3. Remove the length caveat from the user and architecture documentation.
4. Promote `enquiry-response` from partial to supported in
   `docs/ghostty-parity.json`.

## Terminal XTSHIFTESCAPE state query

**Status:** the four-value `mouse-shift-capture` configuration and the
override-independent `always`/`never` capture routing are implemented; exact
terminal overrides for `true`/`false` are blocked on an upstream public query.

Ghostty stores a running program's `XTSHIFTESCAPE` request in the private
`Terminal.flags.mouse_shift_capture` tri-state:

- unset means that the configured `mouse-shift-capture` fallback applies;
- false means that `Shift` releases application mouse capture; and
- true means that `Shift` remains in the encoded mouse protocol.

`Surface.mouseShiftCapture` ignores that terminal state for the configuration
values `always` and `never`. For `true` and `false`, it first honors an
explicit terminal value and otherwise uses the configured fallback.

Public libghostty-vt exposes whether any DEC mouse tracking mode is active, but
does not expose the separate unset/false/true `XTSHIFTESCAPE` state. The public
parser consumes valid `CSI > s`, `CSI > 0 s`, and `CSI > 1 s` input into the
private flag, so the information already exists at the correct terminal-model
boundary. It is unavailable to an embedding frontend after parsing. This was
last verified against official upstream commit
`c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3` on 2026-07-26.

ghostty-qt therefore transports and reloads all four configuration values and
implements the capture-routing branches of `always` and `never` exactly. Its
staged `true` and `false` paths use their configured capture and release
defaults, respectively, for every implemented capture-routing path, but cannot
observe a later terminal override. Captured wheel and buttonless DEC-motion
reporting intentionally remain outside the routing decision, while buttonless
`Ctrl+Shift` hover still consults it for local link eligibility. The parity
ledger keeps `mouse-shift-capture` partial solely until the public state query
is available. The separate delayed Shift-click extension of an existing
selection is implemented locally through the public selection-gesture API and
does not require an upstream change.

### Why ghostty-qt does not inspect PTY bytes

Mirroring `XTSHIFTESCAPE` in the session worker would create a second terminal
parser outside libghostty-vt. It would need to duplicate streaming escape
sequence boundaries, malformed-input handling, the exact point at which a
valid sequence commits, and every reset path. It could also diverge when
Ghostty extends its parser while the Qt byte watcher remains unchanged.

The state cannot be inferred from the public mouse-tracking boolean or from
encoded mouse bytes: tracking and shift capture are independent, and no mouse
event may occur between a terminal request and a local gesture. A local
Ghostty patch would expose the private field but violates this repository's
official-submodule policy. Keeping the configured fallback explicit is safer
than claiming that the conditional modes are exact.

### Required upstream contract

Official libghostty-vt needs an append-only terminal-data query for the current
program-controlled mouse-shift state. The exact API shape remains an upstream
decision, but the result must distinguish all three values:

- no `XTSHIFTESCAPE` override, so the frontend must use its configuration;
- explicit release, produced by valid zero or omitted input; and
- explicit capture, produced by valid one input.

An explicit enum result is preferable to overloading `GHOSTTY_NO_VALUE`,
because unset is successful terminal state rather than unavailable data. A
minimal compatible shape would append a `GhosttyTerminalData` member whose
output type is a new C enum with unset, false, and true values, and support it
through both `ghostty_terminal_get` and `ghostty_terminal_get_multi`.

The query must:

- report the state owned by the same terminal instance that parsed the input;
- reflect the latest valid sequence after `ghostty_terminal_vt_write` returns;
- leave the prior state unchanged after malformed or unsupported parameters;
- return unset for a new terminal and after the same reset operations that
  reset Ghostty's private flag;
- remain independent of whether DEC mouse tracking is currently enabled and
  which screen is active; and
- preserve every existing public enum ordinal and ABI layout.

The public contract should expose terminal state, not accept a frontend
configuration value or return a pre-resolved boolean. `always`/`never` policy
belongs to the embedding surface, while the unset distinction is necessary to
apply the `true`/`false` fallback correctly.

### Upstream acceptance evidence

Public C API tests should verify:

- unset from a newly initialized terminal;
- false after both `CSI > s` and `CSI > 0 s`;
- true after `CSI > 1 s`;
- fragmented input producing the same result as one complete write;
- the latest valid sequence winning;
- unsupported parameters and malformed sequences retaining the prior value;
- terminal reset restoring unset;
- mouse-tracking mode and primary/alternate-screen changes leaving the value
  intact; and
- identical typed results from the single and multi-data getters.

An integration matrix should also compare the public result with Ghostty's
existing `Surface.mouseShiftCapture` resolution for configured `false` and
`true` across unset, explicit-false, and explicit-true terminal states. The
`never` and `always` cases must remain independent of the queried value.

### ghostty-qt follow-up after upstream support lands

Once the query is present in an official, publicly reachable Ghostty commit:

1. Update `GHOSTTY_REVISION` and the official submodule gitlink together.
2. Add a typed tri-state to the adapter's mouse-input snapshot and refresh it
   after terminal writes and resets alongside DEC mouse-tracking state.
3. Publish state changes through the existing worker-to-pane input-mode path;
   do not parse escape bytes or expose a Ghostty handle to the GUI thread.
4. Resolve `true`/`false` from an explicit terminal value first and their
   configured fallback second, while retaining the direct `always`/`never`
   branches.
5. Exercise button press/release, held-button drag, middle/right click, and
   `Ctrl+Shift` link hover/activation across the full configuration and
   terminal-state matrix, while verifying that wheel/fractional scroll and
   buttonless DEC-motion reporting remain unaffected.
6. Promote `mouse-shift-capture` to supported in
   `docs/ghostty-parity.json`.

## Terminal mouse-shape state query

**Status:** Ghostty's initial I-beam, DEC-tracking arrow, Shift override, and
rectangle/link/hidden cursor priorities are implemented; arbitrary
application-requested OSC 22 shapes are blocked on an upstream public query.

Full Ghostty owns one terminal mouse shape. It starts as `text`, enabling DEC
mouse modes 9, 1000, 1002, or 1003 changes it to `default`, disabling those
modes changes it back to `text`, and RIS restores `text`. A recognized
`OSC 22;<name>` request can replace that state with any of Ghostty's 34
W3C-derived shapes. Unknown or empty names leave the prior shape unchanged.

Public `libghostty-vt` exposes only whether any DEC mouse mode is active.
Its private `Terminal.mouse_shape` receives recognized OSC 22 values, but
`GhosttyTerminalData` has no mouse-shape member and the public OSC command-data
API does not expose the parsed name. The similarly named enum and action in
`ghostty.h` belong to Ghostty's full application-runtime API, not the
`ghostty/vt.h` embedding API used here. This was last verified against official
upstream commit `c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3` on 2026-07-26.

There is an additional contract gap: the standalone VT stream handler stores
OSC 22 values but does not mirror full Ghostty's mouse-mode and RIS shape
transitions. Exposing that field without aligning those transitions would
still return the wrong effective state. The existing public mouse-tracking
query also collapses the individual DEC mode bits to an any-mode boolean.
Full Ghostty instead updates one current mouse-event state on each individual
mode transition, so resetting one of multiple overlapping mode bits can select
`text` even while the public any-mode result remains true. That ordering edge
cannot be reconstructed from the current query.

ghostty-qt therefore derives the publicly distinguishable baseline: an I-beam
without raw DEC tracking, an arrow with raw tracking, and an I-beam while
Shift is held under raw tracking. This is exact for the ordinary
non-overlapping tracking path but cannot recover the transition-order edge
above. The existing pane arbiter places
typing concealment, an accepted hyperlink hand, and rectangle-selection
crosshair above that baseline. It intentionally uses raw terminal state rather
than the independent frontend `mouse-reporting` or `mouse-shift-capture`
policies.

### Why ghostty-qt does not inspect PTY bytes

Mirroring OSC 22 outside libghostty-vt would require a second streaming escape
parser. It would have to reproduce fragmented OSC input, BEL versus ST
termination, cancellation and malformed-input behavior, every canonical name
and alias, unknown-name retention, DEC-mode side effects, and RIS ordering.
That duplicate state could silently diverge whenever Ghostty's parser or shape
catalog changes.

A local Ghostty source patch would expose the private field but violates this
repository's official-submodule policy. The base-state implementation is
therefore preferable to claiming partial OSC 22 support through byte
inspection.

### Required upstream contract

Official libghostty-vt needs an append-only typed query for the current
effective terminal mouse shape. A minimal compatible design would:

- define a public C enum containing Ghostty's complete mouse-shape catalog;
- append a `GHOSTTY_TERMINAL_DATA_MOUSE_SHAPE` member whose output type is that
  enum and support it through both `ghostty_terminal_get` and
  `ghostty_terminal_get_multi`;
- report `text` for a new terminal;
- expose the latest recognized OSC 22 request after
  `ghostty_terminal_vt_write` returns, including fragmented input;
- retain the previous value for empty, unknown, malformed, or cancelled
  requests;
- align the standalone VT handler with full Ghostty by applying `default` on
  DEC mouse-mode enable, `text` on disable, and `text` on RIS;
- preserve the latest individual transition semantics when multiple DEC mouse
  mode bits overlap rather than deriving shape from an any-mode reduction;
- preserve the existing public enum ordinals and ABI layouts; and
- keep pointer presentation policy, hyperlink state, and keyboard modifiers
  outside the terminal query.

The query must expose terminal state rather than a Qt or GTK cursor name.
Frontend runtimes remain responsible for choosing the closest native cursor
where their toolkit cannot represent a W3C shape exactly.

### Upstream acceptance evidence

Public C API tests should verify:

- initial `text`;
- every canonical OSC 22 name and every supported xterm/foot alias;
- fragmented OSC input matching a single complete write;
- BEL and ST terminators;
- empty, unknown, malformed, and cancelled requests preserving prior state;
- later valid requests replacing earlier ones;
- each supported DEC mouse mode changing the shape to `default` and its reset
  changing the shape to `text`, including ordering around a custom shape;
- RIS restoring `text`; and
- identical typed results from the single and multi-data getters.

### ghostty-qt follow-up after upstream support lands

Once the query and matching state transitions are present in an official,
publicly reachable Ghostty commit:

1. Update `GHOSTTY_REVISION` and the official submodule gitlink together.
2. Add a project-owned mouse-shape enum to the adapter snapshot and refresh it
   after terminal writes and resets alongside DEC mouse-tracking state.
3. Publish changes through a dedicated worker/controller signal; mouse shape
   is input presentation state, not render-cell metadata.
4. Map every shape to the closest Qt cursor while documenting unavoidable
   collapses such as vertical text and directional resize variants.
5. Keep blank, accepted-link, and modifier-driven rectangle/base transitions
   above the terminal-requested shape according to pinned Surface behavior.
6. Add adapter and pane tests for the complete upstream matrix before
   declaring OSC 22 pointer shapes supported.

## Semantic prompt viewport navigation

**Status:** blocked on an upstream public API.

Public `libghostty-vt` does not currently expose Ghostty's exact
`jump_to_prompt:<i16>` operation or its prompt iterator. Ghostty already has
the required internal prompt traversal:

- `PageList.Scroll.delta_prompt` and `PageList.scrollPrompt` perform the
  compressed-scrollback-safe traversal.
- `Screen.scroll` and the terminal action path already delegate to that
  implementation.
- Ghostty's Linux defaults bind physical `Ctrl+Shift+Up` and
  `Ctrl+Shift+Down` to `jump_to_prompt:-1` and `jump_to_prompt:1`.

The public `GhosttyTerminalScrollViewportTag` currently exposes top, bottom,
row-delta, and absolute-row scrolling, but not prompt-delta scrolling. This was
last verified against official upstream commit
`c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3` on 2026-07-23. The ghostty-qt
submodule remains at that official revision.

### Why ghostty-qt does not scan prompts itself

A public-API-only Qt implementation is technically possible. The scrollbar
data exposes the absolute viewport offset, full-screen grid references can
read historical rows, `GhosttyRowSemanticPrompt` distinguishes prompts from
continuations, and absolute-row scrolling can move to a discovered row.
Ghostty also restores compressed pages when those public grid references are
read.

That fallback would not reuse Ghostty's exact semantic traversal. It would
duplicate private prompt-grouping and direction rules in C++, repeatedly scan
the page list without a public iterator, and require the caller to serialize
the scan against terminal mutations. Public screen-point coordinates are also
limited to `uint32_t`, while Ghostty's history and scrolling types can represent
larger offsets. Changes to Ghostty's continuation or orphan-prompt behavior
could therefore silently diverge from ghostty-qt.

The project intentionally rejects that local scanner in favor of exact,
maintainable parity. The feature remains blocked until upstream exposes the
existing prompt-delta operation through the public terminal API.

### Required upstream contract

The official `libghostty-vt` API needs an append-only prompt-delta viewport
variant that:

- accepts a pointer-sized signed delta;
- treats negative values as jumps toward older prompts and positive values as
  jumps toward newer prompts;
- treats zero as a no-op;
- clamps when no further prompt exists in the requested direction;
- skips OSC 133 prompt-continuation rows;
- returns to the active screen when forward traversal reaches the live area;
- delegates to Ghostty's existing prompt iterator so compressed pages are
  restored and traversed by Ghostty rather than reimplemented by Qt; and
- preserves the existing public enum ordinals and tagged-union ABI.

A minimal compatible shape would append a
`GHOSTTY_SCROLL_VIEWPORT_DELTA_PROMPT` tag, add an `intptr_t delta_prompt`
member to the already padded viewport value union, and route that tag through
`ghostty_terminal_scroll_viewport` to the existing internal
`.delta_prompt` behavior. The exact API shape remains an upstream decision.

### Upstream acceptance evidence

The upstream change should exercise the public C representation with real OSC
133 metadata and verify:

- primary prompts plus `OSC 133;P;k=c` continuations;
- positive and negative multi-prompt jumps;
- zero-delta behavior;
- repeated and oversized jumps clamping at both ends; and
- forward traversal returning to the active screen.

### ghostty-qt follow-up after upstream support lands

Once the API is present in an official, publicly reachable Ghostty commit:

1. Update `GHOSTTY_REVISION` and the submodule gitlink together.
2. Add a typed `PromptDelta` viewport request while reusing the existing
   pane-to-controller-to-worker route.
3. Parse the exact signed `i16` `jump_to_prompt` grammar.
4. Map the request to the new public `libghostty-vt` tag without scanning
   scrollback in Qt.
5. Verify the finalized default `Ctrl+Shift+Up/Down` bindings and their
   consumed key-event behavior.
6. Add adapter tests for OSC 133 prompts, continuations, clamping, and active
   screen return.
7. Promote `jump_to_prompt` in `docs/ghostty-parity.json`; automatic
   shell-script injection is already provided by the pinned helper.

Command notifications remain a separate parity stage. Shell-script injection
and public-semantic prompt-aware close detection are already implemented; the
remaining exact private-state gaps are recorded separately in the parity
ledger.

## Semantic prompt cursor click movement

**Status:** blocked on an upstream public API.

`cursor-click-to-move` is not ordinary mouse-to-cell cursor placement.
Ghostty's private `Surface.maybePromptClick` first checks the terminal's live
OSC 133 semantic-click mode, whether the cursor is at a prompt input area,
prompt ordering, selection/drag state, and the current cursor-key mode. It then
either emits Kitty-compatible SGR click events or asks
`Screen.promptClickMove` for exact left/right input counts across input cells,
soft wraps, and prompt continuations.

Official `libghostty-vt` exposes semantic row/cell metadata, but it does not
expose the live semantic-click mode, `cursorIsAtPrompt`, the prompt-relative
movement calculation, or a composite prompt-click operation. Reconstructing
those decisions from a public grid would duplicate terminal-owned navigation
and could race terminal mutation or diverge as the shell protocol evolves.
This was verified against official upstream commit
`c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3` on 2026-07-29.

The preferred append-only contract is one terminal operation that receives the
released click cell and returns either:

- not handled;
- an owned byte sequence for the host to queue to the PTY; or
- a normalized list/count of cursor-key movements whose normal/application
  encoding is already resolved by Ghostty.

It must retain Ghostty's checks for disabled/none mode, cursor-at-prompt,
selection and drag suppression, clicks before the current prompt,
absolute/relative click-events, soft-wrapped input, continuation rows, clicks
beyond the current input, and live cursor-key mode. After such an API reaches
an official commit, ghostty-qt can transport the finalized boolean live and
invoke the operation from its existing worker-ordered left-release path
without adding a second OSC 133 state machine.

## Semantic screen clearing

**Status:** blocked on an upstream public API.

Ghostty's `clear_screen` binding is a semantic terminal operation, not an
alias for writing a conventional CSI clear sequence. At the official pinned
revision, `Surface.performBindingAction` first reports the action unperformed
on the alternate screen. On the primary screen it delegates to
`Termio.clearScreen`, which atomically:

- clears the active selection and scrollback;
- when the cursor is not at a semantic prompt, erases only content above the
  cursor, clears the screen's Kitty graphics state, and writes nothing to the
  child; or
- when the cursor is at a semantic prompt, erases the complete screen and
  writes one form-feed byte (`0x0c`) to the child so the shell can repaint.

Official `libghostty-vt` exposes neither this composite mutation nor enough
stable primitives to reproduce it exactly. In particular, an embedder cannot
invoke the private prompt test, erase the precise internal ranges, or apply
the operation's Kitty-image and selection policy through the public API. This
was verified against official upstream commit
`c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3` on 2026-07-23.

### Why ghostty-qt does not send an escape sequence

Sending `CSI 2 J`, `CSI 3 J`, or form feed through terminal input would act on
the wrong side of the emulator boundary and would be observably different.
It could not return false for the alternate screen before consuming a
`performable:` binding, would not reproduce the prompt-dependent range, and
would not atomically coordinate scrollback, selection, compressed pages, and
Kitty graphics. It could also send bytes to the child in the non-prompt case,
where Ghostty intentionally sends none.

The project therefore keeps `clear_screen` unavailable rather than carrying a
local Ghostty patch or a Qt approximation.

### Required upstream contract

The official terminal C API needs an append-only operation that delegates to
Ghostty's existing semantic clear implementation and reports one of three
outcomes to the embedding frontend:

- no operation because the alternate screen is active;
- terminal state cleared with no child input required; or
- terminal state cleared and one form-feed byte required for the child.

The operation must preserve Ghostty's exact selection, compressed-scrollback,
cursor/prompt, erase-range, and Kitty-image behavior. The result must be
available synchronously so a frontend can implement Ghostty's performable
keybinding semantics and enqueue any required form feed in order on its PTY
worker. The exact enum and function shape remain an upstream decision, and
existing public enum ordinals and struct layouts must remain ABI-compatible.

### Upstream acceptance evidence

Public C tests should verify:

- alternate-screen no-op with no mutation and an unperformed result;
- primary-screen non-prompt clearing above the cursor only, with no form feed;
- semantic-prompt complete clearing with exactly one form-feed result;
- selection clearing;
- scrollback clearing across restored/compressed pages;
- Kitty graphics deletion in the same cases as the internal operation; and
- stable results when no scrollback, selection, graphics, or visible content
  exists.

### ghostty-qt follow-up after upstream support lands

Once the contract is available in an official, publicly reachable Ghostty
commit:

1. Update `GHOSTTY_REVISION` and the official submodule gitlink together.
2. Parse the exact void `clear_screen` action into the typed pane-action
   pipeline.
3. Invoke the new operation on `SessionWorker` and enqueue form feed only for
   the result that requests it.
4. Return false for the alternate-screen result so `performable:` bindings
   fall through exactly.
5. Exercise local and all/global bindings across primary and alternate panes.
6. Promote `clear_screen` in `docs/ghostty-parity.json` only after the public
   adapter and PTY-ordering tests pass.

## Kitty placement-pin lifetime during eviction and replacement

**Status:** ghostty-qt bounds its own mirrors of fully occluded opaque video
frames; libghostty leaves the screen pin owned by each placement tracked until
terminal teardown when image-budget eviction or external-placement replacement
removes the placement without deinitializing it.

Every ordinary Kitty placement tracks a `PageList.Pin`, and
`ImageStorage.Placement.deinit(screen)` is the operation that untracks it.
The pinned implementation's image-budget eviction removes matching placement
map entries with `removeByPtr()` without first calling that deinitializer.
Likewise, `addPlacement()` assigns a new value over an existing external
`(image_id, placement_id)` entry without deinitializing the placement it
replaces.

This matters for sustained video. mpv 0.41 sends RGB24 transmit-and-display
frames without image or placement IDs. libghostty assigns a new image and
internal placement to every frame, retains their decoded bytes up to
`image-storage-limit`, and then evicts the oldest used images and placement
entries. The byte budget remains bounded, but each evicted frame leaves one
tracked screen pin behind. Long sessions can therefore grow both pin storage
and the work of operations that walk tracked pins.

ghostty-qt's snapshot bridge safely avoids copying fully hidden opaque frames
into Qt CPU images and textures, but it must not send synthetic delete commands
or edit libghostty-owned placement state: doing so would change protocol
semantics, and a later application delete is allowed to reveal a lower
placement in renderer order. This lifetime bug was verified at the pinned
official revision
`c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3` and again at local official
`origin/main` revision `6ad1fe7d8cbda36c77b337a96c9bea8a77883699` on
2026-07-31.

### Required upstream correction

Official Ghostty should deinitialize every placement before eviction removes
its map entry and before insertion replaces an existing external placement.
The internal call chain must make the owning screen available wherever this
cleanup occurs; changing a private function signature is sufficient, and no
new public C API is required.

Eviction tests should repeatedly exceed a small image budget with used
placements and verify that the tracked-pin count returns to the number of live
placements. Replacement tests should repeatedly reuse one non-zero placement
ID and make the same assertion. Both paths should then scroll, resize, clear,
and delete content to exercise tracked-pin traversal before terminal teardown.

### ghostty-qt follow-up after the upstream fix lands

Once the correction is present in an official, publicly reachable commit:

1. Update `GHOSTTY_REVISION` and the official submodule gitlink together.
2. Add a sustained mpv-shaped adapter test whose raw data crosses a deliberately
   small image-storage limit while its Qt snapshot stays bounded.
3. Confirm that deleting the covering frame still reveals the highest retained
   predecessor in renderer order and that tracked-pin count remains bounded
   throughout the session.
4. Remove this entry only after the upstream lifetime regression tests and the
   frontend integration case both pass.

## Expanded Kitty Unicode placement rendering

**Status:** ordinary Kitty placements are implemented; Unicode virtual
placements are blocked on an upstream public API.

Official `libghostty-vt` exposes decoded image bytes, image and storage
generations, ordinary placement iteration, layer ordering, source rectangles,
physical destination sizes, and viewport positions. That is sufficient for
ghostty-qt to render ordinary placements without borrowing a terminal-owned
handle across threads.

Virtual placements are different. A `U=1` placement is only a definition.
Ghostty's renderer privately scans visible cells for U+10EEEE placeholders,
decodes placement and image IDs from combining codepoints, applies per-cell
row bookkeeping, and expands each match into a clipped render fragment.
`ghostty_kitty_graphics_placement_render_info()` intentionally marks a virtual
definition invisible and the public iterator does not expose those expanded
fragments. Reimplementing the placeholder scan in Qt would duplicate
terminal-owned rules and could diverge across reflow, scrollback compression,
wide cells, and future protocol changes.

The preferred append-only contract is an expanded-render-placement iterator
for a terminal viewport. Each result should identify the image and placement
generations, image and placement IDs, z index, viewport cell origin, physical
pixel offset and destination size, and resolved source rectangle. Results must
cover both ordinary and virtual fragments in Ghostty renderer order, or expose
an explicit kind while preserving an equivalent stable sort. All returned
handles and pixel pointers may remain borrowed under the existing
no-terminal-mutation lifetime rule.

Public C acceptance should cover multiple placeholders for one definition,
combining-codepoint IDs, clipping on all viewport edges, wide and spacer
cells, scrolling and reflow, primary/alternate screens, deletion and image
replacement generations, malformed placeholders, and no-placeholder
definitions.

After that contract reaches an official commit, ghostty-qt can replace its
ordinary-only snapshot builder with the expanded iterator, remove the
diagnostic `containsVirtualPlacements` limitation, and add identical RHI and
software rendering tests without changing the Qt layer or texture architecture.

## Exact font resolution and terminal shaping

**Status:** the finalized typography configuration, Qt-owned face resolution,
maximal compatible row shaping, and device-pixel-safe fallback are implemented.
Exact Ghostty face selection, FreeType behavior, synthesis, run construction,
and positioned glyph output require a public renderer-neutral font contract.

The standalone public VT API exposes the authoritative row cells, styles,
selection range, base codepoint, grapheme length, and complete grapheme bytes.
That is sufficient for ghostty-qt to preserve terminal contents and to build a
safe frontend shaping plan. It does not expose the font system used by
Ghostty's generic renderer:

- `font.SharedGridSet` and `font.Collection`, including the embedded fallback
  stack, concrete face indexes, variation coordinates, codepoint resolver,
  native-versus-synthetic role decisions, and FreeType load options;
- `font.shape.RunIterator`, including its exact style, selection, cursor,
  grapheme, spacer, and fallback boundaries;
- `font.Shaper`, its feature application, and its shaping cache; or
- each shaped cell's font index, glyph index, terminal-relative x position,
  and x/y glyph offsets.

The broader `ghostty.h` application runtime owns a complete renderer and
surface model, but adopting it would replace ghostty-qt's Qt scene graph,
session worker, pane, split, and PTY ownership. It is not an incremental public
font API for the standalone terminal.

At official revision `c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3`,
ghostty-qt therefore implements the largest safe frontend-owned subset:

- schema v1 transports Ghostty's finalized ordered feature list, four ordered
  variation lists as exact f64 bit patterns, u21 codepoint maps, three
  synthesis booleans, cursor shaping-break policy, and five FreeType flags;
- a weak-cached immutable GUI-thread font program shares its metric and
  shaping-face arrays across panes, DPRs, and layout-only generations, and
  invalidates every live pane when Qt's font database changes;
- codepoint maps compile in O(n log n) into sorted disjoint
  later-entry-wins intervals and an interned face table, avoiding quadratic
  overlay copies and duplicate resolved faces;
- unrelated, metric-only, shaping-break-only, and transport-only FreeType
  reloads skip font discovery through an effective program key;
- a pure planner forms maximal row runs at every observable text-style,
  selection, font, invisible-cell, defensive ligature, and configured cursor
  boundary while retaining wide spacers with their head;
- Qt shapes those runs with `QTextLayout`; and
- an all-boundary device-pixel check handles ordinary runs without inspecting
  glyph metadata. If only internal caret positions differ, Qt's glyph string
  indexes identify shaping clusters and the renderer instead validates their
  starts plus the run endpoint. Incomplete metadata or a remaining mismatch
  keeps exact per-cell placement for the complete run.

This gives visible feature and ligature support without allowing a platform
shaper to move later cells off-grid. The parity ledger remains conservative:
all four `font-family*` keys, all four `font-style*` keys, `font-feature`, the
four `font-variation*` keys, `font-codepoint-map`, `font-synthetic-style`,
`font-shaping-break`, and `freetype-load-flags` are partial rather than exact.

### Why ghostty-qt does not copy Ghostty's private font stack

Importing `src/font` or `src/renderer/generic.zig` into the project-private
configuration helper would turn an intentionally narrow parser subprocess into
a second renderer runtime with private ABI coupling. The returned face indexes
are meaningful only with the matching `SharedGrid`, and the shaped glyph
indexes require matching face and rasterization state. Reimplementing those
types in C++ would duplicate Ghostty's font discovery, fallback classification,
synthetic transformations, variation validation, HarfBuzz cluster logic,
sprite selection, glyph caching, and backend-specific load policy.

Patching the Ghostty submodule to export those internals would create an
unpublishable local dependency and violate this repository's
official-submodule policy. The submodule therefore remains the unmodified
`https://github.com/ghostty-org/ghostty.git`; the project-private Zig overlay
only reads finalized configuration and does not add a Ghostty commit.

### Required upstream contract

Official Ghostty needs a stable, append-only font and shaping API that can be
used with public standalone render-state cells. The exact ownership and naming
remain upstream decisions, but an embedding renderer needs:

1. An opaque font-grid handle constructed from finalized font family, style,
   size, feature, variation, codepoint-map, synthesis, and backend load
   settings, plus an explicit reload/replacement lifecycle.
2. Authoritative cell metrics at a requested device scale, including width,
   height, baseline, and decoration metrics, derived from the same regular face
   that will shape and rasterize text.
3. A row-shaping operation that accepts the public render-state row, its
   selection range, and an optional logical cursor column, then returns
   immutable run records and shaped glyph records. Each result must identify
   its stable font face, glyph index, terminal cell x, x/y offset, and any
   required presentation flags.
4. A renderer-neutral way to consume each face/glyph pair. This may be a
   public rasterization API with explicit alpha/RGBA bitmap ownership and
   bearings, or another stable contract that does not require a frontend to
   reinterpret Ghostty-private face indexes.
5. A cache-generation identifier so a frontend can safely reuse rasterized
   glyphs and discard them after a font-grid reload without retaining dangling
   handles.

The shaping operation must use Ghostty's existing `RunIterator`, codepoint
resolver, `Shaper`, synthesis policy, and backend load configuration rather
than introducing a second public approximation. Returned storage may be
borrowed for the duration of one call or explicitly owned, but the lifetime,
thread-affinity, and invalidation rules must be unambiguous. Existing public
enum ordinals and struct layouts must remain ABI-compatible.

### Upstream acceptance evidence

Public C tests should compare the new result with Ghostty's generic renderer
for:

- default and repeated feature tags, including implicit defaults and
  later-tag replacement;
- every role's variation list, duplicate tags, unsupported tags, out-of-range
  values, and non-finite values;
- overlapping and missing codepoint-map faces, complete graphemes, ZWJ,
  variation selectors, and regular-face use from bold or italic cells;
- native, disabled, named, and each permitted or forbidden synthetic role;
- every FreeType flag combination supported by the Linux backend;
- selection and all style boundaries, invisible and wide cells, defensive
  `fi`, `fl`, and `st` cases, and cursor-break enabled versus disabled;
- ASCII ligatures, combining clusters, Arabic or other complex shaping,
  bidirectional codepoints on the physical terminal grid, and color glyphs;
- one-shot and fragmented terminal input producing identical row shaping; and
- font reload invalidating old face/glyph handles while a no-op reload
  preserves reusable cache generation.

The tests should assert run offsets, font identity, glyph indexes, cell x
positions, glyph offsets, metrics, and raster bounds—not only the rendered
string.

### ghostty-qt follow-up after upstream support lands

Once this contract exists in an official, publicly reachable Ghostty commit:

1. Update `GHOSTTY_REVISION` and the official submodule gitlink together.
2. Replace `TerminalFontResolver` and Qt-owned feature/variation/map/synthesis
   decisions with the opaque upstream grid while retaining value-only config
   snapshots.
3. Feed the existing worker-authoritative render rows directly into the public
   run shaper; do not expose the terminal handle across threads.
4. Add a retained QSG glyph atlas around the public raster result, keyed by
   upstream grid generation, face, glyph, and device scale.
5. Keep the current dirty-row and scene-node lifetime architecture, but remove
   the `QTextLayout` grid-validation fallback once every glyph comes from the
   authoritative plan.
6. Compare screenshots and structured glyph records across DPRs, splits,
   cursor movement, selection, search, live reload, missing fonts, and complex
   scripts before promoting the affected parity entries to supported.

## Exact clipboard selection formatting

**Status:** mapped plain copying is supported; exact VT, HTML, and mixed
clipboard payloads are blocked on an upstream public formatter contract.

Ghostty's internal `Surface.copySelectionToClipboards` does not use the public
terminal formatter defaults. It constructs a screen formatter with clipboard-
specific state:

- the live `clipboard-codepoint-map`;
- the terminal's effective foreground and background colors;
- the terminal's current 256-color palette;
- soft-wrap unwrapping and the live trailing-space trimming policy; and
- no unrelated terminal-state extras.

`copy_to_clipboard:mixed` first completes the plain representation and then
the HTML representation before publishing either. The mixed HTML output
deliberately omits the terminal's default foreground and background while
retaining cell styling. Automatic copy-on-select uses this mixed format for
both primary-only and primary-plus-standard destinations.

The pinned implementation and its comments currently disagree about codepoint
mapping. `Surface.copySelectionToClipboards` passes the same map through every
formatter mode, and `PageFormatter` applies replacements without a plain-only
guard. Nearby Surface comments say VT and HTML should preserve their native
encoding and should not receive mappings. Upstream must define which behavior
is canonical before exposing it as a public clipboard contract; ghostty-qt
must not choose between implementation and stated intent locally.

The official public artifact can select plain, VT, or HTML output, but neither
of its formatter paths can express that clipboard contract:

- `GhosttyTerminalSelectionFormatOptions` exposes only the output format,
  unwrap policy, trim policy, and optional selection. Its implementation uses
  a terminal formatter with style extras, so VT and HTML include terminal-state
  material that Ghostty's clipboard screen formatter does not emit.
- `GhosttyFormatterTerminalOptions` can suppress those extras, but it cannot
  receive a clipboard codepoint map, effective foreground/background colors,
  or a palette for resolving styled cells.
- The colors and palette can be queried separately through
  `ghostty_terminal_get`, but the public formatter cannot consume them.

This was verified against the official pinned submodule commit
`c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3` on 2026-07-29.

### Why ghostty-qt does not approximate styled copies

Using the public selection formatter directly would produce syntactically
valid VT and HTML, but observably different clipboard bytes. Querying colors
separately and prepending VT state or parsing and rewriting generated HTML
would duplicate Ghostty's private formatter policy. It would also remain
fragile as palette resolution, hyperlink styling, formatter envelopes, and
codepoint mapping evolve upstream.

ghostty-qt therefore keeps the public plain selection formatter and applies
the finalized ordered map to its Unicode output on `SessionWorker`. That local
post-format pass is byte-equivalent to pinned `PageFormatter` for plain text:
it visits Unicode codepoints once, searches entries newest-first, permits
string expansion or deletion, and emits U+FFFD for non-scalar u21 replacement
values. Every plain selection-copy route shares the result, while title, URL,
search, and terminal-file consumers remain outside the map.

This does not choose a styled policy. ghostty-qt does not advertise the public
terminal formatter's different VT/HTML output as clipboard parity, and
automatic copies still contain mapped plain text rather than Ghostty's
required mixed plain-plus-HTML payload. Destination routing,
primary-selection fallback, live trimming/mapping, and worker-atomic
copy/clear remain implemented independently of that upstream blocker.

### Required upstream contract

The official public API needs a stable, append-only way to format a selection
with Ghostty's clipboard semantics. The exact API shape is an upstream
decision, but it must support either a complete mixed MIME result or enough
clipboard-specific options to produce the representations independently:

- the active selection or an explicit `GhosttySelection` value/range;
- plain, VT, HTML, and mixed plain-plus-HTML behavior;
- raw byte output with no `QString` or NUL-termination requirement;
- soft-wrap unwrapping and configurable trailing-space trimming;
- a defined codepoint-mapping policy for every representation, resolving the
  pinned implementation/comment disagreement described above;
- the effective foreground/background and current palette for explicit VT and
  HTML output;
- mixed HTML's intentional omission of default foreground/background colors;
- no cursor, active-hyperlink, palette-setup, or other terminal-state extras
  absent from `Surface.copySelectionToClipboards`; and
- a distinguishable no-selection result, valid empty representation, and
  formatter failure.

If mixed output is returned as multiple MIME entries, the call must either
produce the complete set or fail without exposing a partial result. Any new C
struct fields must follow libghostty-vt's existing size-versioned ABI rules.

### Upstream acceptance evidence

The public C tests should compare the new contract with
`Surface.copySelectionToClipboards` for:

- plain, VT, HTML, and mixed output from the same styled selection;
- default colors, palette-indexed colors, RGB colors, text attributes, and
  OSC 8 hyperlinks;
- non-ASCII text and exact raw bytes;
- soft-wrapped and hard-wrapped lines with trimming enabled and disabled;
- clipboard codepoint mappings across every representation, with acceptance
  expectations chosen explicitly after resolving the pinned implementation/
  comment disagreement;
- mixed HTML omitting only the default foreground/background envelope;
- no selection versus a valid empty selection; and
- all-or-none mixed formatting on allocation or formatting failure.

### ghostty-qt follow-up after upstream support lands

Once the contract is available in an official Ghostty commit:

1. Update `GHOSTTY_REVISION` and the official submodule gitlink together.
2. Parse `copy_to_clipboard` into typed plain, VT, HTML, and mixed formats,
   with a missing parameter defaulting to mixed.
3. Carry one value-only clipboard payload through pane, controller, and worker;
   never send `QMimeData` across the session-thread boundary.
4. Format the complete payload on `SessionWorker`, emit it, and only then
   apply explicit-copy selection clearing.
5. Make copy-on-select always request mixed output without clearing.
6. Build one complete `QMimeData` per destination on the GUI thread, retaining
   Linux primary-selection fallback and independent ownership. VT is raw
   `text/plain`; HTML is bare `text/html`; every plain representation is
   installed first as `text/plain;charset=utf-8` and then as bare
   `text/plain`, with identical bytes. Install every mixed representation in
   one `setMimeData` call.
7. Add raw-byte, MIME, charset, trimming, destination, lifecycle, and failure-
   atomicity tests before promoting the affected parity entries.

## Normalized standalone VT effects and policy controls

**Status:** dynamic light/dark scheme reporting is implemented through the
existing public callbacks and mode helpers. Title-report policy, desktop
notifications, progress reports, command-finished notifications, color-report
format, and initial grapheme-width policy remain blocked on standalone public
`libghostty-vt` contracts.

ghostty-qt links the public standalone terminal library described by
`ghostty/vt/terminal.h`. Ghostty's broader embedded application API in
`ghostty.h` has application-runtime action types for some of these events, but
adopting that runtime would replace the project's terminal/session ownership
model rather than extend the standalone terminal it already uses.

At official revision `c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3`, the
standalone stream has the following gaps:

- It parses OSC 9 and OSC 777 desktop notifications, then discards the
  normalized `show_desktop_notification` action without a host callback.
- It parses ConEmu OSC 9;4 progress reports, then discards the normalized
  progress action without exposing its state or percentage.
- It records OSC 133 semantic prompt rows, but does not publish Ghostty's exact
  command-start/command-finished lifecycle or its parser-owned C-to-D duration.
- It answers CSI 21 t directly from the terminal's internal OSC title. The
  frontend cannot apply Ghostty's default-false `title-report` security gate or
  substitute the configured base title that full Ghostty reports.
- It answers OSC 4, OSC 10, and OSC 11 color queries internally without a host
  option for Ghostty's `none`, `8-bit`, or `16-bit` report formats.
- Its construction options do not accept Ghostty's `legacy` versus `unicode`
  grapheme-width policy for a new terminal. Mode 2027 remains terminal-owned,
  so resolving widths later in Qt would not be equivalent.

The affected configuration keys are:

- `title-report`;
- `desktop-notifications`;
- `progress-style`;
- `notify-on-command-finish`;
- `notify-on-command-finish-action`;
- `notify-on-command-finish-after`;
- `osc-color-report-format`; and
- `grapheme-width-method`.

### Why ghostty-qt does not inspect PTY bytes

A second parser in `SessionWorker` would need to duplicate Ghostty's fragmented
OSC/CSI parsing, BEL and ST termination, cancellation, malformed-input
handling, safe UTF-8 normalization, OSC 133 metadata, reset behavior, query
reply ordering, and mode interactions. It could emit a duplicate reply after
libghostty-vt already handled the same sequence, and it would silently diverge
as Ghostty's parser evolves.

Inferring command completion from rendered semantic rows is also insufficient.
Rows do not preserve the exact C/D timer, missing-marker behavior, exit-code
normalization, or ordering needed for the documented notification threshold.
The project therefore keeps these items explicitly blocked rather than
installing a parallel byte observer.

### Required upstream contracts

Official standalone `libghostty-vt` should add append-only, synchronous host
effects for:

1. A desktop notification with borrowed, explicit-length title and body bytes.
2. A progress report with Ghostty's normalized remove, set, error,
   indeterminate, and pause states plus an optional clamped percentage.
3. Command started and command finished, with the finished effect carrying the
   normalized optional exit status and parser-owned elapsed duration. Exposing
   these two effects is narrower and more stable than exposing all OSC 133
   syntax.

These effects should follow the existing callback contract: they occur during
`ghostty_terminal_vt_write`, are non-reentrant, retain borrowed data only for
the callback, and preserve terminal input order.

Title reporting additionally needs either:

- a terminal option that gates CSI 21 t and a setter for the host-visible base
  title; or
- a normalized report-title callback that lets the host apply the gate and
  return the exact base title synchronously.

Color reporting needs a construction/runtime option matching Ghostty's
`none`, `8-bit`, and `16-bit` enum before the internal OSC query path emits a
reply. Grapheme width needs a construction-time `legacy`/`unicode` option that
feeds the same terminal width logic as full Ghostty while retaining mode 2027
as the terminal-owned override.

Every new enum member, terminal option, callback, and value struct must follow
the library's existing append-only ABI rules.

### Upstream acceptance evidence

Public C tests should cover:

- OSC 9 and OSC 777, empty and non-empty titles, safe UTF-8 normalization,
  BEL/ST termination, fragmentation, cancellation, and multiple ordered
  notifications in one write;
- every OSC 9;4 progress state, absent and overflowing percentages, removal,
  fragmentation, and ordered replacement;
- OSC 133 C/D pairing, repeated C, D without C, missing/malformed/out-of-range
  exit status, exact duration ownership, reset, and interleaved terminal
  output;
- CSI 21 t disabled by default, enabled empty/non-empty base titles, a
  configured title masking OSC updates, and exact response bytes;
- OSC 4/10/11 under none, 8-bit, and 16-bit reporting, including palette and
  current default-color changes; and
- both initial grapheme-width methods, mode 2027 set/reset precedence,
  combining clusters, emoji modifiers, and width-sensitive cursor movement.

### ghostty-qt follow-up after upstream support lands

Once the required contracts are present in an official, publicly reachable
Ghostty commit:

1. Update `GHOSTTY_REVISION` and the official submodule gitlink together.
2. Copy effect payloads immediately on `SessionWorker` and associate them with
   stable `PaneId` values; never expose a terminal handle or borrowed pointer
   to the GUI thread.
3. Keep focus, live configuration, duration thresholds, bell decisions, and
   progress expiry atomic on the worker. Perform desktop notification and
   presentation calls on the GUI thread.
4. Resolve a notification click through the current `PaneId`, no-op after
   closure, and otherwise select the correct tab/split, focus it, and activate
   its existing window.
5. Pass title-report, color-report, and grapheme-width policy through typed
   adapter options rather than parsing terminal bytes.
6. Add adapter, worker-ordering, pane-lifetime, live-reload, focus, rate-limit,
   and activation tests before promoting the affected parity entries.
