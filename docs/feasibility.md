# Feasibility and stack decision

## Decision

The practical stack for this Linux-only project is **C++23 + Qt Quick/QML**, with
Ghostty built by **Zig 0.16.0** and consumed through the stable C-shaped
`libghostty-vt` API.

This division follows the native boundary of both dependencies: Qt's object,
thread, scene-graph, and QML APIs are C++, while Ghostty owns its Zig build and
exports opaque handles plus value structs for embedders. The resulting bridge
is small and does not require a custom language binding runtime.

## Alternatives considered

| Host stack | Feasibility | Assessment |
| --- | --- | --- |
| C++23 + Qt Quick/QML | High | Recommended. Direct access to Qt, straightforward C ABI calls into libghostty, mature Linux PTY APIs, and the least integration code. |
| Zig host + Qt C++ shim | Medium-low | Zig is excellent for building Ghostty, but Qt still requires a substantial C++ ownership, signal, and QML registration shim. This adds a second application-level language boundary without removing C++. |
| Rust + Qt bindings | Medium | Technically workable, but introduces binding/code-generation risk around QQuickItem and Qt's threaded object model while libghostty is already a C ABI. It does not improve the critical render or PTY paths for this scope. |
| Pure QML/JavaScript | Low | Cannot directly own libghostty handles, nonblocking PTY lifecycle, or a high-throughput terminal render path. A native plugin would recreate the C++ architecture anyway. |
| Qt Widgets instead of Qt Quick | High | Viable for a traditional desktop terminal, but less suitable for a modern composited renderer and evolving tab/split chrome. Qt Quick exposes the public QSG text and geometry nodes used by the implemented GPU path. |

## Linux-only impact

Restricting the target to Linux and Wayland materially improves feasibility:

- one PTY implementation (`forkpty`, `TIOCSWINSZ`, process groups);
- one Qt window-system backend and input model;
- no ConPTY, AppKit, or X11 abstraction layer;
- a generated build-tree terminfo database can be passed directly to children;
- packaging and GPU qualification can focus on common Linux/Wayland drivers.

## Proven by the MVP

The implementation validates the risky integration points rather than only
scaffolding them:

- Ghostty builds at a pinned revision with exact Zig-version enforcement and
  links statically into a Qt executable.
- A real nonblocking PTY drives Ghostty parsing on a worker thread, including
  query callbacks and final-output draining during child exit.
- Ghostty render state crosses to Qt as owned value updates; an initial full
  frame is followed by dirty-row replacements. Wide cells, styles, selection,
  cursor, and scrollback metadata are represented.
- A `QQuickItem` renderer shapes maximal compatible runs with `QTextLayout`.
  OpenGL and Vulkan render exact one-glyph-per-cell printable ASCII through a
  compact terminal-owned CPU Alpha8 atlas, one explicit RGBA coverage texture,
  and retained indexed row batches; public `QSGTextNode` objects preserve the
  general and software fallback. Batched colored geometry handles backgrounds,
  selection, cursors, and decorations.
- Key, mouse, focus, paste, and tracked selection-gesture paths use Ghostty's
  encoders and terminal modes.
- A narrow project-owned Zig/C matcher imports Ghostty's pinned default URL/path
  expression and vendored Oniguruma engine without expanding libghostty-vt's
  dependency surface. C++ receives only UTF-8 byte ranges, which the adapter
  maps to tracked logical terminal cells across graphemes, soft wraps, and
  reflow.
- A pane-local scene-graph link preview reuses the tracked hover destination,
  safely bounds/escapes/elides arbitrary bytes, and relocates from left to
  right when entered without another matcher scan.
- Compiled QML provides tabs, recursive splits, dialogs, and Wayland startup.
- CTest covers direct libghostty behavior and an end-to-end PTY session.

## Remaining engineering risks

The project is feasible as a terminal MVP, but production parity is a larger
effort. The main risks are renderer performance, the evolving libghostty API,
advanced text/graphics support, and distribution:

- Renderer-v1 has the intended hybrid QSG/GPU glyph path, dirty-row worker/UI
  transport, persistent row text containers, and retained cell-derived RHI
  geometry per visible row. Sparse updates plan and shape only damaged rows and
  commit only their GPU geometry; the software fallback keeps global solid-node
  pools after benchmarking showed that row-node traversal was counterproductive
  there. Compatible text runs reduce layout count with exact-position fallback
  for unsafe runs, while the narrower ASCII path removes native text submissions
  without replacing Qt shaping. Reproducible host reports and a noise-aware
  comparator gate structural work and full-profile timing; collecting
  representative baselines and reducing global invalidation cost remain the
  principal renderer performance risks.
- The libghostty revision should remain pinned; upgrades need focused ABI/API
  review and protocol regressions.
- Color emoji and exact Ghostty font shaping still require explicit renderer
  policy beyond VT parsing. Ordinary Kitty graphics placements now use
  libghostty's public decoded-image and placement APIs with Qt RHI/software
  paths. Opaque overlap culling bounds Qt's mirrors of mpv-style frame churn,
  while libghostty retains orphaned tracked pins during storage eviction as an
  upstream lifetime risk; Unicode virtual placements remain blocked on expanded
  public viewport fragments. Search uses the cooperative public-grid
  foundation. OSC 8 and the
  built-in `link-url` matcher now share tracked hover, copy, and open behavior,
  stable live-output handling, coalesced pointer queries, and mutation-safe
  activation. The `true`/`false`/`osc8` preview policy is implemented as a
  frontend-only overlay and reloads without querying the worker. User-defined
  `link` expressions/actions remain blocked because the pinned parser's
  `RepeatableLink.parseCLI` returns `error.NotImplemented`;
  pathological logical lines and regex searches deliberately fail closed at
  bounded cell/byte and Oniguruma retry limits. Exact OSC grouping by hyperlink
  ID also remains limited by the public C API, which exposes only the
  destination URI.
- Runtime terminfo lookup is relocatable and covered by a staged-install test.
  A second staged contract covers desktop, D-Bus, and systemd user-service
  activation, a scalable project icon, and AppStream metadata;
  distribution-specific packages remain.
- Headless smoke tests use Qt's software scene graph. The hardware RHI path has
  a reproducible host matrix for OpenGL/Vulkan readbacks, invariant counters,
  device/DPR metadata, a production-window swapchain/frame-swap probe, and
  strict baseline/candidate comparison with loaded userspace graphics-stack
  fingerprints.
  Final compositor output and representative performance baselines still need
  interactive collection on target Wayland compositors and GPU drivers.

None of these risks invalidates the selected stack. They are incremental work
on top of a functioning boundary, rather than blockers requiring a rewrite.
