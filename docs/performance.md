# Performance

This document is the single reference for performance measurement, current
optimization boundaries, and proposed work. Architecture describes ownership;
benchmark executables and their `--help` output remain authoritative for exact
scenario and option catalogs.

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

This is a competent baseline. There is no evidence yet for replacing it with
`io_uring` or an additional I/O thread on Linux.

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
- image storage is packed straight-alpha RGBA rather than separate color and
  alpha planes;
- the retained shader pipeline reuses pipelines, bindings, uniform storage,
  and at most two intermediate textures.

Structural changes such as font, DPR, backend, render context, or incompatible
grid geometry still require broader invalidation. Compositor output, blur,
color management, and presentation timing remain host-level measurement
boundaries.

### Search

Physical search-row snapshots store the screen row once and one 16-bit column
per emitted UTF-8 byte. The worker reconstructs a full search coordinate only
when the KMP scanner consumes that byte. This row-local structure-of-arrays
boundary reduces byte-to-cell mapping storage from 8 to 2 bytes per byte while
preserving wide-character, combining-grapheme, wrap, newline, and viewport
semantics.

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

## Remaining I/O optimization roadmap

The end-to-end PTY benchmark and bounded read gathering now cover the primary
Linux read path. Use `perf stat` or `perf record -g` for further CPU attribution
and `strace` only for syscall topology because tracing perturbs latency. Do not
port Ghostty's additional gather/parser thread or buffer ring unless these
measurements identify remaining kernel backpressure.

### 1. Isolate cold filesystem work

Terminal file actions currently format and write the artifact synchronously on
the session worker. Very large history or a stalled temporary filesystem can
delay PTY reads and pane teardown.

A future implementation can snapshot/format terminal-owned data on the worker
and perform the filesystem write in a bounded cancellable job. Completion must
rejoin the worker's ordered action barrier so a later paste or keybinding action
cannot overtake the file effect. Streaming formatting additionally requires an
upstream API.

## Deferred ideas

The following are not justified without new measurements:

- `io_uring` for per-pane PTYs;
- a second permanent I/O thread per pane;
- delayed batching of ordinary keystrokes;
- a broad renderer or PTY rewrite;
- frontend SIMD for work dominated by Qt shaping or libghostty parsing.

Prefer incremental changes that preserve the current ownership model. If
profiling shows libghostty dominates a workload, optimize or report that layer
upstream rather than duplicating its parser in the Qt frontend.
