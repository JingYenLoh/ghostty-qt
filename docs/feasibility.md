# Feasibility and stack decision

## Decision

The practical stack for this Linux-only project is **C++20 + Qt Quick/QML**, with
Ghostty built by **Zig 0.15.2** and consumed through the stable C-shaped
`libghostty-vt` API.

This division follows the native boundary of both dependencies: Qt's object,
thread, scene-graph, and QML APIs are C++, while Ghostty owns its Zig build and
exports opaque handles plus value structs for embedders. The resulting bridge
is small and does not require a custom language binding runtime.

## Alternatives considered

| Host stack | Feasibility | Assessment |
| --- | --- | --- |
| C++20 + Qt Quick/QML | High | Recommended. Direct access to Qt, straightforward C ABI calls into libghostty, mature Linux PTY APIs, and the least integration code. |
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
- A `QQuickItem` renderer uses public `QSGTextNode` objects with `QtRendering`
  for distance-field glyph atlases on hardware RHI backends. Per-cell
  `QTextLayout` preserves exact grid placement, while batched colored geometry
  handles backgrounds, selection, cursors, and decorations.
- Key, mouse, focus, paste, and tracked selection-gesture paths use Ghostty's
  encoders and terminal modes.
- Compiled QML provides tabs, recursive splits, dialogs, and Wayland startup.
- CTest covers direct libghostty behavior and an end-to-end PTY session.

## Remaining engineering risks

The project is feasible as a terminal MVP, but production parity is a larger
effort. The main risks are renderer performance, the evolving libghostty API,
advanced text/graphics support, and distribution:

- Renderer-v1 has the intended QSG/GPU glyph path and dirty-row worker/UI
  transport, but still rebuilds the retained root's child nodes and lays out
  text per cell when presenting an update. Persistent row nodes and larger
  compatible runs will be needed to reduce CPU overhead in high-throughput
  workloads.
- The libghostty revision should remain pinned; upgrades need focused ABI/API
  review and protocol regressions.
- Color emoji, ligatures across cells, Kitty graphics, hyperlinks, and search
  each require explicit UI/render policy beyond VT parsing.
- Runtime terminfo lookup is relocatable and covered by a staged-install test;
  desktop integration metadata and distribution-specific packages remain.
- Headless smoke tests use Qt's software scene graph. The hardware RHI path
  needs interactive visual and performance qualification on representative
  Wayland compositors and GPU drivers.

None of these risks invalidates the selected stack. They are incremental work
on top of a functioning boundary, rather than blockers requiring a rewrite.
