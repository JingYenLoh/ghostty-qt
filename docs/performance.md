# Performance

This document records current performance architecture, reproducible evidence,
and the profiling gates for work that remains actionable. Architecture
describes ownership; benchmark executables and their `--help` output remain
authoritative for exact scenario and option catalogs.

## Principles

- Measure Release builds before changing an architecture.
- Preserve latency and correctness, not only throughput.
- Compare the same host, Qt version, RHI backend, device, DPR, dimensions,
  warmup, and iteration count.
- Use deterministic work counters and readbacks to detect accidentally skipped
  rendering.
- Treat short timing runs as advisory. Enforce timing only with sufficiently
  long, repeatable samples.
- Optimize frontend-owned transport and rendering separately from
  libghostty-owned parsing and terminal mutation.

Performance changes should include a benchmark or profile that identifies the
cost being removed and a regression test for any new batching, ownership, or
ordering rule.

## Current design

### Session I/O

Each pane has one session thread. PTY reads are nonblocking and notifier-driven,
use a 64 KiB stack buffer, process at most 1 MiB per normal event-loop
activation, and coalesce frame publication over 8 ms. Fragmented reads below
1 KiB are gathered into parser submissions of up to 4 KiB. Larger reads are
submitted immediately, preserving the producer/consumer pipeline for saturated
output. At `EAGAIN`, the worker submits a trailing partial batch and retries
once, allowing the producer to refill without turning an activation into a
poll. The byte cap protects same-pane control requests from an unbounded output
loop.

Writes use a borrowed synchronous view from the libghostty callback through
the session worker. When the FIFO is empty, the worker writes that view
directly to the PTY; only an unwritten suffix after a short write or `EAGAIN`
crosses into owning FIFO storage. Successful queued prefixes advance an index
rather than moving the remaining bytes after every short write, and
compaction is amortized. Once the PTY reports backpressure, later replies are
appended in order without repeating a known-to-fail syscall; the write
notifier retries the accumulated suffix when the descriptor becomes writable.

Qt and libghostty intentionally meet at a text-encoding boundary. Qt key and
input-method events expose text as `QString` UTF-16, while
`ghostty_key_event_set_utf8` accepts a borrowed `char *` plus a UTF-8 byte
length and does not take ownership. The key encoder therefore converts the
text once with `QString::toUtf8()` immediately before the synchronous
libghostty call and keeps the resulting `QByteArray` alive through that call.
Passing `QString` storage directly would supply UTF-16 bytes with the wrong
encoding and length. Transporting all UI text as UTF-8 instead would move
decoding into Qt-facing editing, IME, and composition work, so this single
boundary conversion is required rather than an optimization target. Search
similarly converts its UI `QString` once because the worker scans UTF-8 row
snapshots and maintains byte-to-cell mappings; queued Qt value copies on both
paths remain implicitly shared.

`bench-terminal-session-io` re-executes itself as a raw deterministic PTY
writer and reports transport topology, throughput, frame latency, and event-loop
delay. On the development host, a 16 MiB ANSI stream emitted in 64-byte writes
retained about 60.6 MiB/s while reducing parser submissions from roughly 710k
to 43k and p99 event-loop gaps from 74.9 ms to 13.3 ms. Set
`GHOSTTY_QT_PTY_READ_BATCHING=legacy` for benchmark A/B diagnosis; it is not a
supported user setting.

The benchmark's `replies` workload sends ordered ENQ bursts, verifies every
reply byte in the child, and reports write calls, backpressure, direct bytes,
FIFO-copy bytes, and backing-allocation counts. In a representative Release
run of fifteen 4,096-query bursts with 256-byte replies, the direct path cut
write calls from 63,870 to 38,801, `EAGAIN` results from 25,697 to 1,734,
FIFO-copied bytes from 15.0 MiB to 6.4 MiB, and FIFO backing allocations from
36,589 to 95. Sequential replies which do not backpressure report zero FIFO
copies and allocations. `GHOSTTY_QT_PTY_WRITE_DIRECT=legacy` restores the
always-buffered path for benchmark A/B diagnosis only.

### Frame transport

Visible cells cross the worker/UI boundary as contiguous row payloads. Their
16 boolean attributes, color-source fields, underline style, and 1/2-cell span
share one explicitly masked 32-bit word. The three cell colors use a separate
opaque RGB8 value with an explicit invalid sentinel instead of three 16-byte
`QColor` objects; alpha remains a renderer-owned presentation concern. Together
these layouts reduce `TerminalCell` from 112 to 48 bytes on the current x86-64
Qt ABI and lower queued-update, retained-frame, and detach-copy pressure without
relying on C++ bitfield layout.

The retained `TerminalFrame` stores an implicitly shared vector payload per
physical row. A render pass can retain a shallow frame snapshot while the next
update detaches only the small outer row table and installs the incoming dirty
row payloads. Clean rows remain shared; neither partial nor full updates copy
`TerminalCell` objects while being merged into the retained frame. The
renderer traverses each row payload directly, avoiding flat-index division in
its cell scan.

`bench-terminal-frame-materialization` holds a render snapshot during every
apply and reports row-table detach, row-header copy, row-payload reuse, cell
allocation, and cell-copy counters. At 240x80 on the development host, the
row-sharded apply medians were 0.74 us for one dirty row, 0.83 us for four
dirty rows, and 2.02 us for a full frame, versus 55.02 us, 63.98 us, and
171.98 us with flat whole-grid detachment. All row-sharded cases reported zero
cell-payload allocations and zero copied terminal cells. A partial update
still copies 80 small row headers (1,920 bytes) when a snapshot shares the
outer table; this bounded cost is the remaining ownership tradeoff.

A CPU profile of the materialization half found two redundant libghostty C
calls per ordinary cell: the resolved foreground and background getters
re-read style and palette data already fetched by the frontend. Ordinary text
cells now resolve those colors from the existing snapshot values; only
Ghostty's compact background-only cells need one conditional raw-cell query.
The benchmark contract reports this topology alongside timing: ASCII uses two
cell-data calls per materialized cell, the alternating extended-grapheme
corpus uses 2.5, and the background-only corpus uses three. Representative
240x80 Release medians fell from 12.98 to 12.08 us for one dirty row, 41.73 to
39.33 us for four dirty rows, and 1,059.03 to 1,023.11 us for a full frame.

The next partial-frame profile exposed a fixed global-color cost: every render
copied libghostty's 256-entry RGB palette and compared it against 256 `QColor`
values. Libghostty's partial-dirty contract guarantees that only rows changed
and no global state, including colors, changed. The adapter now retains the raw
color snapshot from the last successfully published full frame and skips both
the color-state query and palette comparison on partial frames. Benchmark
contract 4 asserts zero color-state queries for partial updates and one for a
full frame. On the same 240x80 Release workload, the one-dirty-row median fell
from 12.03 to 11.31 us and the four-dirty-row median from 39.33 to 38.55 us;
full frames still refresh the complete color snapshot.

### Renderer

The retained renderer already avoids the largest terminal-frontend costs:

- ordinary terminal updates transport and rebuild dirty rows rather than the
  full grid;
- row dependency masks skip cell scans and shaping for unrelated global paint
  changes;
- RHI solid geometry, printable-ASCII glyph batches, and atlas resources are
  retained across compatible updates;
- Qt text nodes are created lazily for complex shaping and the software path;
- exact per-cell fallback layouts are reconstructed from run boundaries only
  after grid-fit rejection instead of being retained for every ordinary cell;
- text runs are planned while the renderer already visits a dirty row, avoiding
  a second traversal and a transient 128-byte staging object per visible cell;
- shaping compatibility keys compare packed RGB8 cell colors directly instead
  of expanding two colors per visited cell into `QColor` temporaries;
- validated glyph plans omit unused UTF-16 source indexes, and retained glyph
  quads store the float edges ultimately uploaded to the GPU, reducing their
  x86-64 layouts from 40 to 32 bytes and from 80 to 48 bytes respectively;
- Kitty textures and placement nodes survive redraw, movement, and compatible
  replacement;
- duplicate-aware Kitty placement reconciliation builds its exact-match index
  eagerly but defers the four lower-priority fallback indexes until an exact
  implicit-placement match actually fails;
- image storage is packed straight-alpha RGBA rather than separate color and
  alpha planes;
- the retained shader pipeline reuses pipelines, bindings, uniform storage,
  and at most two intermediate textures.

Structural changes such as font, DPR, backend, render context, or incompatible
grid geometry still require broader invalidation. Compositor output, blur,
color management, and presentation timing remain host-level measurement
boundaries.

#### Copy and conversion audit

A Release OpenGL profile on the development host audited full-frame,
dirty-row, Kitty-movement, and retained custom-shader paths for deep copies and
repeated representation changes. The measured opportunities were implemented
as follows:

| Site | Change and evidence | Result |
| --- | --- | --- |
| Packed cell color expansion | A rebuilt cell expanded its packed foreground and background into `QColor` even when adjacent cells had identical colors. `QColor::fromRgb` accounted for 3.51% of sampled cycles in the ASCII full-frame scenario and 1.33% in the dirty-row scenario. Separate one-entry foreground and background caches now key expansion by `TerminalCellColor`. | Combined with glyph color reuse below, three pinned 500-frame runs reduced 240x80 full-frame CPU recording from 8,892.1 to 8,096.9 us (-8.9%) and total CPU from 10,268.8 to 9,478.2 us (-7.7%). Aggregate task-clock fell 7.7%, cycles 7.9%, and retired instructions 2.2%. The 5,000-frame dirty-row comparison reduced 240x80 recording from 346.8 to 337.3 us (-2.7%) and total CPU from 384.5 to 374.8 us (-2.5%). |
| Glyph vertex color conversion | Geometry emission converted and premultiplied every glyph color although glyphs from the same run normally share a color; the production call path accounted for about 1.09% of sampled full-frame cycles. One geometry write now retains the last logical color and its four uploaded bytes. | Included in the full-frame and dirty-row measurements above. Existing mixed alpha, native/linear blending, glyph-batch readback, explicit-color, selection, search, and global-paint contracts preserve output and work counters. Rectangle batches have the same conversion shape, but profiles still do not show enough cost to justify adding state there. |
| Kitty placement snapshots | Every changed snapshot copied each resolved placement into a second vector used only by the render test probe, while the required materialized vector repeatedly grew. Production now keeps only retained placement nodes, probe geometry is derived from those nodes on demand, and materialization reserves snapshot cardinality. | Across five pinned 5,000-frame movement runs, aggregate task-clock fell 6.2%, cycles 6.5%, and retired instructions 5.6%. Median recording fell from 115.5 to 103.7 us (-10.2%) at 120x40 and from 160.5 to 149.7 us (-6.7%) at 240x80; total CPU fell 9.0% and 6.1% respectively. Texture, node, geometry, material, eviction, and placement-probe contracts were unchanged. |
| Custom-shader uniform snapshots | `TerminalCustomShaderUniforms` is 4,592 bytes. A newly scheduled render used to publish a time/frame snapshot and immediately copy it again to refresh base state; it also reconverted all 256 palette colors on every base refresh. The updates now share one mutable snapshot before publication, and the converted palette is retained until its `QVector<QColor>` value changes. | This removes one whole-struct copy from each newly scheduled shader frame and makes the usual unchanged-palette refresh a shared-vector comparison. A separate trial that copied the immutable payload directly to mapped RHI storage and patched matrix/opacity in place was not retained: a five-repeat 1,000-frame reverse-order comparison increased whole-benchmark instructions by 0.9% and did not improve task-clock. The compiler-friendly local RHI struct copy remains. |

The same audit found no redundant deep cell copy in worker-to-render frame
delivery. The queued connection provides the required cross-thread ownership,
Qt containers remain implicitly shared, dirty rows install shared payloads,
and installing an update while a render snapshot exists detaches only the
small outer row table described above.
Owned grapheme text is also required because libghostty's render callback data
cannot outlive the callback. Kitty/background pixel-format conversion occurs
once per changed asset or texture generation and supplies the straight versus
premultiplied representation required by the selected material path.

Renderer benchmarks compile the render test probe into their pane support.
Profiles must therefore exclude its full-grid `QVector<QColor>` construction
and snapshot copies: in the dirty-row profile these appeared as 5.57% in the
vector constructor and 5.26% in `memmove`, but neither exists in the production
target. Likewise, full-frame `QColor::fromString` and part of
`QColor::toRgb()` came from benchmark fixture construction rather than the
renderer. Attribute call stacks before treating generic copy or conversion
symbols as production work.

The `kitty-implicit-reorder` renderer scenario alternates the order of 512
otherwise stable implicit placements. In five pinned 1,000-frame OpenGL
Release runs on the development host, lazy fallback indexes reduced aggregate
task-clock from 1,104.73 ms to 954.43 ms and retired instructions from 11.843
billion to 10.236 billion. Median CPU recording fell from 337.0 us to 271.9 us
at 120x40 and from 391.4 us to 324.6 us at 240x80, while texture, node,
geometry, and material counters remained unchanged.

### Search

Physical search-row snapshots store the screen row once and one 16-bit column
per emitted UTF-8 byte. The worker reconstructs a full search coordinate only
when the KMP scanner consumes that byte. This row-local structure-of-arrays
boundary reduces byte-to-cell mapping storage from 8 to 2 bytes per byte while
preserving wide-character, combining-grapheme, wrap, newline, and viewport
semantics.

Both the canonical and provisional KMP passes keep their needle-sized cell
history in fixed-capacity contiguous rings. The scanners append one cell for
every byte and only need the oldest cell when a match completes; a segmented
`std::deque` added allocation and pop-front machinery without providing useful
growth or iterator stability.

On a pinned 50,000-row resident-scrollback workload with 21 measured searches,
the rings reduced whole-process retired instructions by 1.0–1.2%, branch misses
by 11.2–11.4%, and task-clock by 0.8–1.2% across two final counter runs against
the same deque baseline. Canonical completion medians were 1.0–1.3% lower.

### Shell startup

Shell-integration finalization retains successful equivalent preparations in a
bounded process-wide cache. Concurrent panes with the same launch identity
share one in-flight helper process; failures are delivered to those waiters but
are not retained, so a later pane retries. The LRU keeps at most 32 results,
with a 1 MiB per-result limit and an 8 MiB retained-result payload budget.

The key covers the canonical request, timeout, complete helper-process
environment, helper executable, its revision-matched private `libghostty`, and
the resource nodes that the pinned setup code probes. Filesystem identities are
checked again after a miss and before accepting a hit. Ambiguous inherited
environments, loader injection, helpers outside the expected runtime layout,
and inaccessible identities bypass the cache. Results observed while files are
unstable are returned but not retained.

On the development host, representative Release runs measured median
preparation around 7.0–7.5 ms uncached and 0.8–0.9 ms on a validated warm hit.
An eight-pane identical cold burst launched one helper and coalesced seven
waiters, while eight distinct requests launched concurrently. Warm hits still
serialize and hash the request, recheck filesystem identity, and return an
isolated implicitly shared result.

## Data-structure decisions

The recurring hot paths were audited for container access pattern, cardinality,
allocation behavior, and ownership. The following decisions prevent broad
container substitutions from becoming unaudited performance work:

| Area | Current decision | Evidence or next measurement gate |
| --- | --- | --- |
| Frame cell storage | Keep implicitly shared `QVector` rows behind a small outer row table. | The materialization benchmark reports zero cell copies and allocations during retained-frame apply. Revisit only if row-header detachment becomes visible in a profile. |
| Renderer row caches | Keep separate, reusable contiguous vectors for rectangles, background layers, and glyph foregrounds. | Dirty-row rendering consumes these streams independently and retains their capacities. Trial a packed RGBA representation only if profiles identify retained color bandwidth or footprint; compare full-frame, dirty-row, cursor, selection, and search scenarios. |
| Glyph atlas | Keep hashed glyph lookup and contiguous rasterization/packing inputs. | Lookup is keyed and occurs for every eligible glyph; deduplication and packing occur only during atlas rebuild. A `QSet` in the rebuild path would be a semantic cleanup, not a demonstrated hot-path improvement. |
| Search | Keep contiguous match ranges, bit-packed viewport masks, 16-bit row-local byte columns, and the fixed-capacity cell-history rings described above. | These structures match append, indexed navigation, binary-search, mask-composition, and rolling-window access respectively. Do not reserve matches from total row count: short needles can make that estimate both inaccurate and needlessly large. |
| Kitty placement reconciliation | Keep ordered signature maps, with only the exact-match map eager and four fallback maps lazy. | The `kitty-implicit-reorder` scenario measures the lazy construction win. A trial replacing the ordered maps with broad hashing increased retired instructions by about 7% without improving task time, so hashing is closed unless key shape or cardinality changes. |
| PTY write queue | Keep one `QByteArray` plus a consumed-prefix cursor. | Direct writes and amortized compaction already avoid copies and allocations on the uncontended path; the replies benchmark exposes write calls, copied bytes, allocation count, and backpressure. |
| Shell-startup cache | Keep hash lookup plus a linear least-recently-used victim scan. | The cache is capped at 32 entries and filesystem validation/helper execution dominate a miss. A linked LRU would add nodes and mutation to every hit to optimize a bounded eviction scan. |
| Font-program cache | Keep the thread-local contiguous weak-entry vector. | Live cardinality is bounded by active typography programs and lookup is outside frame recording. Add indexing only if a font-reload/startup profile attributes material time to this scan. |

The remaining data-structure experiments are deliberately workload-gated:

1. Keybinding trie nodes store child entries contiguously and lookup performs
   physical, Unicode, then catch-all passes. Before adding per-kind spans or a
   hash index, add a benchmark covering the shipped bindings, large user root
   tables, named tables, leaders, keypad aliases, and unmatched ordinary input.
   Preserve physical-key priority and modifier-key self matching.
2. Regex-link resolution uses hash sets to deduplicate byte-to-cell mappings,
   while the OSC 8 viewport index uses sparse per-row column vectors. Benchmark
   hover resolution and dirty-frame maintenance with both long/dense and normal
   sparse links before trying order-preserving adjacent deduplication or dense
   row bitsets.
3. Kitty snapshot occlusion and scene texture retention use ordered sets/maps
   keyed by cover geometry and image generation. Extend the Kitty benchmarks
   with many distinct images, overlapping opaque placements, replacement, and
   eviction before comparing flat or hashed indexes; the common one-image path
   is too small to justify a substitution by inspection.
4. Retained renderer colors remain `QColor` because presentation can introduce
   alpha and the Qt scene graph consumes `QColor`; the one-entry packed-input
   caches above close the demonstrated uniform-run conversion cost. Revisit a
   broader packed retained representation only if a new profile identifies
   color bandwidth or footprint after accounting for color-space,
   minimum-contrast, faint-text, and alpha-blending semantics.

## Building benchmarks

Benchmarks are opt-in and excluded from CTest:

```sh
cmake --preset release -DGHOSTTY_QT_BUILD_RENDER_BENCHMARKS=ON
cmake --build --preset release -j"$(nproc)"
```

The shell-integration benchmark also requires the default shared Ghostty
configuration helper. It removes loader-injection variables so its cached cases
measure the eligible production path.

| Executable | Measures |
| --- | --- |
| `bench-ghostty-shell-integration` | Uncached, cold/warm cached, identical-burst, and distinct-burst shell preparation |
| `bench-terminal-pane-renderer` | Full-frame, dirty-row, cursor, search, glyph-batch, and Kitty scene-graph work |
| `bench-terminal-custom-shader-compiler` | Cold shader baking and content-cache hits |
| `bench-terminal-custom-shader-rhi` | Retained versus legacy multi-pass shader recording and GPU work |
| `bench-terminal-kitty-graphics` | Kitty protocol replacement and isolated RGB/RGBA/grayscale materialization |
| `bench-terminal-frame-materialization` | Ghostty VT snapshots plus retained-frame application while a render snapshot holds row payloads shared |
| `bench-terminal-search` | Visible-result latency, canonical history scan, cancellation, and recompression |
| `bench-terminal-session-io` | End-to-end PTY reads/replies, parser submissions, write copies, query latency, ordering, backpressure, and event-loop fairness |
| `bench-terminal-backdrop` | Background asset preparation and software composition |

Representative invocations:

```sh
./build/release/tests/bench-ghostty-shell-integration

./build/release/tests/bench-terminal-pane-renderer \
    --graphics-api software --warmup 20 --iterations 200

QT_QPA_PLATFORM=wayland \
./build/release/tests/bench-terminal-pane-renderer \
    --graphics-api opengl --warmup 200 --iterations 200

QT_QPA_PLATFORM=wayland \
./build/release/tests/bench-terminal-pane-renderer \
    --graphics-api vulkan --warmup 200 --iterations 200

./build/release/tests/bench-terminal-custom-shader-compiler \
    --cold-iterations 5 --warm-iterations 100

./build/release/tests/bench-terminal-kitty-graphics \
    --pixel-format rgba32 --width 640 --height 360 \
    --warmup 10 --iterations 100

./build/release/tests/bench-terminal-kitty-graphics \
    --workload materialization --pixel-format rgba32-opaque \
    --width 1920 --height 1080 --warmup 10 --iterations 100

./build/release/tests/bench-terminal-frame-materialization \
    --workload all --columns 160 --rows 48 --dirty-rows 4 \
    --warmup 10 --iterations 100

./build/release/tests/bench-terminal-search \
    --rows 25000 --viewport-rows 32 --warmup 1 --iterations 5

perf stat -e task-clock,instructions,cycles,branches,branch-misses \
    taskset -c 0 ./build/release/tests/bench-terminal-search \
    --rows 50000 --viewport-rows 32 --warmup 3 --iterations 21 --resident

./build/release/tests/bench-terminal-session-io \
    --bytes 16777216 --chunk-bytes 64 --corpus ansi \
    --warmup 2 --iterations 7 --mode both

./build/release/tests/bench-terminal-session-io \
    --workload replies --queries 4096 --response-bytes 256 \
    --query-burst 4096 --warmup 2 --iterations 9 --write-mode both
```

Use each executable's `--help` and, where available, `--list-scenarios` for the
complete interface. `LIBGL_ALWAYS_SOFTWARE=1` selects a Mesa software device
behind an RHI backend; it is different from Qt Quick's software scene graph.

Offscreen RHI benchmarks serialize frame completion and do not model
swapchain-present pipelining. They are useful for controlled renderer work and
GPU timestamps, not end-user frame pacing by themselves.

Use `perf stat` or `perf record -g` for CPU attribution. Use `strace` only to
verify syscall topology because tracing perturbs latency. If a profile shows
libghostty dominates a workload, optimize or report that layer upstream rather
than duplicating its parser in the Qt frontend.

## Wayland renderer qualification

Run the qualification harness from a real Wayland session:

```sh
./scripts/qualify-wayland-renderer.py --profile quick
```

The quick profile checks OpenGL/Vulkan availability, deterministic benchmark
invariants, readbacks, device identity, synthetic scales, and a shown
production-window swapchain. A genuinely unavailable optional backend is
reported as a skip; use repeated `--require-backend` options when a host must
support a particular API.

The harness rejects llvmpipe, lavapipe, and other QRhi CPU devices by default.
Use `--allow-software-device` only for an explicit non-production diagnostic.

Collect an enforceable baseline with the full profile on an otherwise idle
host:

```sh
./scripts/qualify-wayland-renderer.py \
    --profile full \
    --skip-build \
    --output-directory tmp/renderer-full-baseline
```

Reports are written atomically below `tmp/renderer-qualification` by default.
They include benchmark results, Qt/RHI device and output context, artifact
hashes, and mapped graphics-library fingerprints.

Compare compatible reports with:

```sh
./scripts/compare-renderer-qualification.py \
    tmp/renderer-baseline/results.json \
    tmp/renderer-candidate/results.json \
    --output tmp/renderer-candidate/comparison.json
```

The comparator rejects incompatible workloads or environments before
evaluating counters and timing. Quick-profile timings are advisory; the full
profile supplies the sample counts required for enforced noise-aware gates.

Qualification returns `0` on success, `1` on failure, and `77` when the
requested Wayland/backend environment is unavailable. The comparator returns
`0` for no enforced regression, `1` for a regression, `2` for malformed or
incompatible evidence, and `77` for a skipped candidate report.

## RenderDoc

Capture one warmed custom-shader or pane-renderer frame with:

```sh
./scripts/capture-custom-shader-renderdoc.sh \
    vulkan retained effect-only 8

./scripts/capture-terminal-pane-renderdoc.sh \
    vulkan kitty-replacement
```

Vulkan is preferred because RenderDoc injection is generally more reliable
than native Wayland EGL capture. The benchmarks use explicit capture markers
around an offscreen frame, so inspect the named output texture rather than
expecting a presented backbuffer. The pane benchmark output is named
`ghostty-qt terminal-pane benchmark output`.

Captures are written below ignored `tmp/renderdoc`. Use Release builds and the
host GPU for meaningful GPU inspection.

Two environment variables provide targeted A/B controls:

- `GHOSTTY_QT_CUSTOM_SHADER_PIPELINE=legacy` selects the former nested shader
  implementation for diagnosis; the retained pipeline is the default.
- `GHOSTTY_QT_DISABLE_GLYPH_BATCH=1` forces the general Qt text path instead of
  the printable-ASCII batch.

They are diagnostic controls, not supported user configuration.

## Idle session monitoring

Child exit and interactive-shell activity use independent event paths. On
pidfd-capable Linux hosts, pidfd readiness is the only recurring child-exit
observer; a 100 ms `waitpid(WNOHANG)` timer is enabled only when `pidfd_open`
is unavailable or denied.

Accepted process-starting input arms one activity reconciliation at the end of
the 250 ms submission grace period. PTY output and semantic prompt markers
reconcile immediately. A 250 ms foreground-group probe repeats only while an
unintegrated foreground job owns the PTY, then stops when the shell regains the
foreground group. A settled idle prompt therefore has no recurring `waitpid`
or `tcgetpgrp` calls on the pidfd path.

The session-worker regression probe verifies the pidfd/fallback timer topology,
the zero-timer idle state, and foreground-job re-arming. The existing activity
suite additionally covers delayed integrated-shell startup, silent foreground
jobs, same-process shell builtins, semantic prompt transitions, rejected input,
direct commands, and child-exit policy.

## Auxiliary IPC

The global-shortcut portal derives its complete registry before mutating the
active session. When a reload produces an identical registry, the existing
session, signal subscriptions, generation, and action map remain installed.
The private-D-Bus regression probe verifies that both an exact reload and one
with unrelated non-global keybindings retain the session with zero additional
`CreateSession`, `BindShortcuts`, or session `Close` calls. A changed registry
still replaces the session atomically, and inactive or disconnected states
still retry registration.

## Configuration reloads

Invalid configuration no longer starts a helper or frontend filesystem load
every five seconds indefinitely. Each configuration service exponentially
backs automatic retries from 5 seconds through a bounded 5 minute maximum
while no external reload event occurs. A file-watcher event, manual request,
color-scheme change, or successful load resets the delay so repairs remain
responsive. For a continuously broken configuration, this reduces automatic
retry transactions during the first hour from 720 to 16.

The service regression probes use reduced deterministic bounds to verify the
initial delay, exponential step, maximum cap, and independent manual/watcher
reset paths in both the shared Ghostty and frontend configuration domains.

## Terminal artifact persistence

Terminal file actions snapshot and format libghostty-owned state on the
session worker, then move every filesystem operation to a dedicated pool with
at most two concurrent jobs process-wide. Directory creation and hardening,
file creation, permission validation, write/flush/close, and canonicalization
therefore cannot block PTY reads or pane teardown.

Each worker installs an ordered placeholder before dispatch. Later
user-originated PTY writes and terminal-file completions remain behind that
placeholder, so a raw path paste and a following keybinding cannot overtake
the artifact. Terminal protocol replies bypass this user-action barrier and
remain live. Shutdown invalidates the completion target immediately; the
background job observes cancellation between filesystem stages, and the
owned temporary directory removes an artifact unless the worker commits its
successful result.

The session-worker regression probe verifies asynchronous completion, exact
`BEFORE -> path -> AFTER` PTY ordering, persistent 0600 artifacts under 0700
directories, and cancellation without artifact leakage or a teardown join.
Formatting itself stays on the worker because libghostty's public formatter
requires terminal ownership; streaming it would require an upstream API.
