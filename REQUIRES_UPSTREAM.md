# Features requiring upstream Ghostty changes

ghostty-qt consumes Ghostty only through the official `ghostty-org/ghostty`
submodule and its public APIs. Do not carry local Ghostty source patches in this
repository. When a parity item needs an upstream change, describe the missing
public contract here and do not mark the unavailable behavior supported. Use
`partial`, `planned`, or `blocked_upstream` as appropriate until an official
Ghostty commit provides it.

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
`f3c9a2b7262a989ba7e9408d00471fda8f788d16` on 2026-07-18. The ghostty-qt
submodule remains at the revision recorded by the parent repository.

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
7. Promote `jump_to_prompt` in `docs/ghostty-parity.json` with a note that
   automatic shell-script injection is still not implemented.

Shell-script injection, command notifications, and prompt-aware close
detection are separate parity stages and are not implied by prompt navigation.

## Exact clipboard selection formatting

**Status:** unmapped plain copying is supported; mapped plain output and exact
VT, HTML, and mixed clipboard payloads are blocked on an upstream public
formatter contract.

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
`c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3` on 2026-07-18.

### Why ghostty-qt does not approximate styled copies

Using the public selection formatter directly would produce syntactically
valid VT and HTML, but observably different clipboard bytes. Querying colors
separately and prepending VT state or parsing and rewriting generated HTML
would duplicate Ghostty's private formatter policy. It would also remain
fragile as palette resolution, hyperlink styling, formatter envelopes, and
codepoint mapping evolve upstream.

ghostty-qt therefore keeps the exact plain path for the currently supported
configuration slice, where `clipboard-codepoint-map` is unavailable and thus
empty. It does not advertise mapped plain output or the public terminal
formatter's different styled output as clipboard parity. The current
destination routing, primary-selection fallback, live trimming, and worker-
atomic copy/clear lifecycle remain useful partial behavior. The parity ledger
records that automatic copies currently contain plain text rather than
Ghostty's required mixed plain-plus-HTML payload.

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
