# Features requiring upstream Ghostty changes

ghostty-qt consumes Ghostty only through the official `ghostty-org/ghostty`
submodule and its public APIs. Do not carry local Ghostty source patches in this
repository. When a parity item needs an upstream change, describe the missing
public contract here and leave the feature planned until an official Ghostty
commit provides it.

## Semantic prompt viewport navigation

**Status:** blocked on an upstream public API.

ghostty-qt cannot currently implement Ghostty's `jump_to_prompt:<i16>` action
through public `libghostty-vt`. Ghostty already has the required internal
prompt traversal:

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

1. Update the submodule pin and every recorded revision together.
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
