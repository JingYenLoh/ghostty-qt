# Project status

ghostty-qt is a usable Linux/Wayland terminal frontend under active
development. It covers the core terminal, workspace, configuration, and Linux
desktop feature set, but does not yet claim complete Ghostty frontend parity.

## Supported target

- Linux and Wayland only; X11 and other operating systems are outside scope.
- Qt Quick 6.10 or newer with LayerShellQt 6.6.4 or newer.
- Public `libghostty-vt` for terminal behavior, plus a private helper around
  the pinned Ghostty application parser.
- Portable and Linux Ghostty configuration settings where behavior maps to Qt.
- Final Ghostty overrides and Qt-specific policy in `ghostty-qt/config`,
  without GTK-prefixed aliases.

Implemented areas include PTYs and process lifecycle, retained rendering, tabs
and recursive splits, multiple windows, quick terminal, configuration and
keybindings, search, links, clipboard writes, shell integration, terminfo,
desktop activation, systemd integration, custom shaders, and ordinary Kitty
graphics placements.

## Parity source of truth

[`ghostty-parity.json`](ghostty-parity.json) inventories every configuration
key, keybinding action, and CLI action in the pinned Ghostty revision. Entries
are classified as `supported`, `partial`, `planned`, `blocked_upstream`, or
`not_applicable`. The classifications are deliberately conservative.

Run the revision and inventory check with:

```sh
python3 scripts/check-ghostty-parity.py
```

The checker requires `GHOSTTY_REVISION`, the submodule checkout, CMake pin, and
manifest to describe the same commit. See [Development and CI](development.md)
for the intentional upgrade procedure.

## Remaining gaps

- **Text and fonts:** Qt owns final shaping. Exact Ghostty fallback, synthesis,
  generated box/icon glyphs, positioned shaping, and color emoji need a wider
  public font/render contract.
- **Kitty graphics:** ordinary RGB/RGBA transmissions, including mpv-style
  RGB24 continuation frames, are supported. Unicode virtual placements and
  safe handling of some upstream eviction lifetimes require libghostty
  changes.
- **Search:** the frontend performs a cooperative public-grid scan. Some
  cross-boundary and exact formatting semantics differ from Ghostty's private
  search implementation.
- **Clipboard and files:** terminal clipboard writes support
  `allow`/`deny`/`ask`, and plain terminal-file actions work.
  Terminal-requested reads, exact styled VT/HTML selection copies, and styled
  VT/HTML file variants need upstream APIs.
- **Semantic and diagnostic state:** prompt navigation, semantic clearing, and
  several terminal-state queries and notification/reporting semantics remain
  partial or blocked on public libghostty state.
- **Links:** OSC 8 and the pinned default URL/path matcher work. User-defined
  link expressions remain blocked because the pinned upstream parser does not
  implement their CLI grammar.
- **Internationalization:** QML source strings are largely marked, but the
  application does not yet build or load Qt translation catalogs. Ghostty's
  pinned gettext catalogs can supply its command-palette text and seed reviewed
  exact frontend matches; the intended ownership and packaging are documented
  in [Internationalization](internationalization.md).
- **Qualification and distribution:** renderer correctness is covered across
  software, OpenGL, and Vulkan, but compositor-specific blending, blur, color
  management, and presentation timing still require representative hardware
  runs. Checkout-local Arch, Debian, and Fedora convenience package recipes are
  available for build testing, but upstream-ready distribution packages are
  not yet provided. Saved sessions and a theme editor are also outstanding.

Exact upstream contracts and acceptance criteria live in
[Features requiring upstream Ghostty APIs](../REQUIRES_UPSTREAM.md).
Renderer state, benchmarks, and optimization work are summarized in
[Performance](performance.md).

## Scope exclusions

X11, macOS, iOS, FreeBSD, GTK/libadwaita implementation details, and
platform-specific behavior without a meaningful Linux/Wayland/Qt mapping are
recorded as not applicable rather than unfinished.
