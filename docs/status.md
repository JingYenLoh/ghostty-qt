# Project status

ghostty-qt is a usable but still evolving Linux/Wayland frontend. It uses
public `libghostty-vt` for terminal state and a narrow private helper around
the pinned Ghostty application parser. It is not yet a drop-in replacement for
every Ghostty frontend feature.

## Supported target

- Linux only.
- Wayland only; X11 is outside the parity target.
- Qt Quick 6.8 or newer with LayerShellQt.
- Portable and Linux Ghostty configuration names where their behavior maps to
  this frontend.
- Qt-specific settings in a separate `ghostty-qt/config` domain instead of
  accepting `gtk-*` aliases.

The project already covers the core terminal, renderer, tabs and splits,
multiwindow lifecycle, quick terminal, configuration, keybindings, search,
links, clipboard writes, shell integration, terminfo, desktop activation,
ordinary Kitty graphics placements, custom-shader post-processing, and
substantial appearance behavior.

## Parity source of truth

[`ghostty-parity.json`](ghostty-parity.json) inventories every configuration
key, keybinding action, and CLI action in the pinned Ghostty revision. Each
entry is explicitly classified as:

- `supported`;
- `partial`;
- `planned`;
- `blocked_upstream`; or
- `not_applicable`.

The ledger is deliberately conservative. “Partial” means useful behavior
exists but some upstream semantics or interface are incomplete; it is not
silently counted as full parity.

Run the inventory and revision check with:

```sh
python3 scripts/check-ghostty-parity.py
```

The check requires the Ghostty checkout, `GHOSTTY_REVISION`, CMake pin, and
manifest to refer to the same commit. See [Development and CI](development.md)
for intentional upgrade procedure.

## Notable remaining gaps

- Exact font fallback, synthesis, generated box/icon glyphs, positioned
  shaping, and color emoji cannot be reproduced through the current public
  terminal API. Qt owns the final shaping result.
- Kitty graphics renders ordinary placements, including foreground and
  background z layers. Direct RGB24 continuation streams used by mpv are
  covered end to end through OpenGL RHI; exact opaque overlap culling keeps
  their Qt-side steady-state mirror to the visible frame, while every mirrored
  asset uses one packed straight-alpha RGBA image and one retained texture.
  Unicode virtual placements used by applications such as yazi still need
  expanded viewport data from `libghostty-vt`. Upstream must also fix the
  tracked screen-pin lifetime of budget-evicted placements.
- Search is a cooperative frontend implementation over public terminal
  snapshots. It does not use Ghostty's private `xev`-dependent search thread
  and retains some documented paging and responsiveness differences.
- Terminal-originated clipboard writes support `allow`, `deny`, and `ask`.
  Terminal-requested clipboard reads remain blocked by the public API, and
  exact styled VT/HTML selection copies need additional upstream formatting
  support.
- Semantic prompt navigation, semantic screen clearing, some terminal-state
  queries, and several notification/reporting behaviors require public
  Ghostty APIs that do not yet exist.
- User-defined `link` expressions are not implemented because the pinned
  upstream parser does not implement their CLI grammar; the parity ledger
  records this as blocked upstream rather than planned frontend work.
- Renderer damage and text nodes are retained per row, as is cell-derived solid
  geometry on RHI backends; the software fallback uses pooled global nodes.
  Production GPU and compositor output still requires interactive qualification
  on real Wayland systems.
- Custom shaders require Qt Quick's OpenGL or Vulkan RHI backend. Other
  backends keep terminal output unfiltered and expose a configuration
  diagnostic; final multi-pass output and frame cost still require
  qualification on the target GPU and Wayland compositor.
- Background-image loading currently keys its weak cache by finalized path,
  size, and millisecond modification time, then reopens that path for decode.
  The deferred hardening frontier is to open each source once, derive a Linux
  file identity with nanosecond timestamps from that handle, decode the same
  handle, and cover same-size/same-mtime and atomic-replacement races.
- Saved sessions, a theme editor, systemd readiness notification, a project
  icon, AppStream metadata, and distribution packaging remain future work.

The exact API gaps and acceptance criteria are maintained in
[Features requiring upstream Ghostty changes](../REQUIRES_UPSTREAM.md).
Renderer and configuration tradeoffs are described in
[Architecture](architecture.md).

## Scope exclusions

X11, macOS, iOS, FreeBSD, GTK/libadwaita implementation details, and
platform-specific behaviors without a meaningful Linux/Wayland/Qt mapping are
recorded as not applicable rather than as unfinished work.
