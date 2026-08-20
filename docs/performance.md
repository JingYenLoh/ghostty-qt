# Performance opportunities

This document is the backlog for performance work that has not been
implemented. Landed architecture and historical benchmark results do not
belong here. Remove an item when its optimization lands or profiling closes
it; retain the supporting benchmark contract with the implementation.

Every experiment must use a Release build, compare identical environments and
workloads, report deterministic work counters alongside timings, and preserve
latency, ordering, and rendering correctness. Short timing runs are advisory.

## Retained-frame row-table ownership

A partial terminal update can detach and copy the outer table of row headers
when a renderer snapshot still shares it. Cell payloads remain shared, so this
is bounded by viewport height, but very tall grids may make the header copy
visible.

Profile one-row and four-row updates at normal and deliberately tall viewport
sizes. If the outer-table detach is material, compare a chunked row table or a
small mutation journal against the current representation. A viable change
must keep full-frame performance stable and retain zero cell-payload
allocations and zero copied `TerminalCell` objects during partial apply.

Use `bench-terminal-frame-materialization` for the ownership and work-counter
contract, then confirm any end-to-end effect with the dirty-row scenarios in
`bench-terminal-pane-renderer`.

## Renderer color representation and conversion

Retained renderer colors may be candidates for a packed representation if a
new profile attributes meaningful time or memory bandwidth to `QColor`
storage. Any trial must account for color-space conversion, alpha,
minimum-contrast adjustment, faint text, selection, search, and the Qt scene
graph boundary; reducing object size alone is not sufficient evidence.

Rectangle batches also have repeated color-conversion opportunities. Add a
one-entry logical-to-uploaded color cache only if a full-frame or dirty-row
profile shows that conversion on a production call stack. Validate software
rendering plus OpenGL and Vulkan readbacks, including mixed alpha and linear
blending.

Use `bench-terminal-pane-renderer` for full-frame, dirty-row, cursor,
selection, and search workloads. Use the renderer qualification comparator on
an otherwise idle Wayland host before accepting timing claims.

## Keybinding trie lookup

Keybinding trie nodes store child entries contiguously and lookup checks
physical, Unicode, then catch-all triggers. Large user tables may benefit from
per-kind spans or an index, but no benchmark currently demonstrates that the
lookup is material.

Before changing the representation, add a benchmark covering shipped
bindings, large root and named tables, leaders, keypad aliases, and unmatched
ordinary input. Preserve physical-key priority, modifier-key self matching,
shared-prefix behavior, and replay semantics. Compare spans and hashing
against the existing contiguous scan using both time and allocation counters.

## Link mapping and viewport indexes

Regex-link resolution uses hash sets to deduplicate byte-to-cell mappings;
OSC 8 viewport lookup uses sparse per-row column vectors. Two possible
experiments remain:

- order-preserving adjacent deduplication for regex mappings; and
- dense row bitsets for link-heavy viewport maintenance.

First add workloads for ordinary sparse links, long links, dense overlapping
links, hover resolution, and dirty-frame maintenance. Reject any structure
that improves the dense case by adding measurable allocation or scan cost to
the common sparse case.

## Kitty scene indexes

Kitty snapshot occlusion and texture retention use ordered containers keyed by
cover geometry and image generation. Flat or hashed indexes may help scenes
with many distinct images and overlapping placements, but the common
one-image case is too small to justify a substitution by inspection.

Extend the Kitty benchmarks with opaque overlap, implicit-placement reorder,
replacement, deletion, and eviction across both small and large scene
cardinalities. Compare task time, retired instructions, allocations, retained
textures, node reuse, and readback checksums. Preserve stable ordering and
duplicate-placement semantics.

Use both `bench-terminal-kitty-graphics` and the Kitty scenarios in
`bench-terminal-pane-renderer`.

## Font-program cache lookup

The thread-local font-program cache is expected to remain small, but typography
reloads or unusually broad fallback sets could make its linear weak-entry scan
material. Add indexing only after a startup or font-reload profile attributes
meaningful time to the scan.

Any experiment must include stale weak-entry cleanup, repeated pane creation,
font-family fallback, variation and feature changes, and configuration reload.
Measure cache cardinality and hit/miss cost rather than inferring a win from
asymptotic lookup complexity.

## Feature-minimized libghostty-vt builds

The pinned upstream exposes `-Dvt-features`, but ghostty-qt currently relies on
snapshot and glyph-protocol behavior in addition to render-state APIs. Revisit
a minimized feature set only if upstream dependency granularity can preserve
all consumed semantics.

Measure library and executable size, link time, cold startup, and resident
memory against the all-enabled build. A candidate must pass the complete
Debug, Release, and sanitizer suites; size reduction does not justify silently
changing terminal behavior.

## Measurement entry points

Build opt-in benchmarks with:

```sh
cmake --preset release -DGHOSTTY_QT_BUILD_RENDER_BENCHMARKS=ON
cmake --build --preset release -j"$(nproc)"
```

Use each benchmark's `--help` and, where available, `--list-scenarios` for its
current interface. Use `perf stat` or `perf record -g` for CPU attribution and
`strace` only for syscall topology. Offscreen RHI timings do not model
swapchain presentation; qualify renderer changes on a real Wayland session
with `scripts/qualify-wayland-renderer.py` and compare compatible reports with
`scripts/compare-renderer-qualification.py`.

For GPU attribution, capture a warmed frame with the existing RenderDoc helper
scripts. If a profile shows libghostty dominates a workload, optimize or
report that layer upstream instead of duplicating its parser or terminal state
in the Qt frontend.
