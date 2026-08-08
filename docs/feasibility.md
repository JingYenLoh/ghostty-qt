# Stack decision

## Decision

ghostty-qt uses **C++23 and Qt Quick/QML** as its application stack. Ghostty is
built with the exact Zig toolchain required by the pinned revision and consumed
through its public C-shaped `libghostty-vt` API.

Qt's object model, scene graph, threading, native event handling, and QML type
system are C++ APIs. Ghostty already exports opaque handles and value
structures suitable for a narrow C++ adapter, so another application-language
binding layer would add complexity without removing C++ from the critical
paths.

## Alternatives considered

| Host stack | Assessment |
| --- | --- |
| C++23 + Qt Quick/QML | Selected. Direct Qt access, a narrow libghostty boundary, and mature Linux PTY support. |
| Zig + a Qt C++ shim | Workable, but the shim would still own most application and rendering behavior while adding another ownership boundary. |
| Rust + Qt bindings | Workable, but binding and code-generation risk does not improve the PTY or render paths for this project. |
| Pure QML/JavaScript | Insufficient for libghostty ownership, PTY lifecycle, and the renderer without recreating a native plugin. |
| Qt Widgets | Suitable for traditional desktop chrome, but less flexible than Qt Quick for retained GPU rendering, overlays, and split layouts. |

## Linux and Wayland scope

The deliberate Linux/Wayland-only target keeps the system boundary small:

- one PTY and process-group implementation;
- one Qt window-system and input backend;
- direct use of Wayland, XKB, portals, LayerShellQt, and Linux cgroups;
- no ConPTY, AppKit, GTK, or X11 abstraction layer;
- qualification and packaging focused on Linux graphics stacks.

## Outcome

The implemented application has validated the selected architecture across PTY
lifecycle, threaded libghostty integration, keyboard and pointer input,
scene-graph rendering, tabs and splits, configuration, and desktop activation.
The current application validates the stack as a long-term implementation,
not merely an integration experiment.

Remaining work is incremental compatibility, performance qualification, and
upstream API integration. It is tracked in [Project status](status.md), the
[parity manifest](ghostty-parity.json), [Performance](performance.md), and
[Features requiring upstream Ghostty APIs](../REQUIRES_UPSTREAM.md); none of
the known gaps calls for changing the selected stack.
