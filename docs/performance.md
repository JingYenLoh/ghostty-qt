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
activation, and coalesce frame publication over 8 ms. The cap protects
same-pane control requests from an unbounded output loop.

Writes use an offset FIFO. Successful prefixes advance an index rather than
moving the remaining bytes after every short write; compaction is amortized.
The write notifier is enabled only while the PTY is backpressured.

This is a competent baseline. There is no evidence yet for replacing it with
`io_uring` or an additional I/O thread on Linux.

### Frame transport

Visible cells cross the worker/UI boundary as contiguous row payloads. Their
16 boolean attributes, color-source fields, underline style, and 1/2-cell span
share one explicitly masked 32-bit word. This reduces `TerminalCell` from 112
to 80 bytes on the current x86-64 Qt ABI and lowers both queued-update and
retained-frame cache pressure without relying on C++ bitfield layout.

The retained `TerminalFrame`, however, still stores every row in one flat
`QVector<TerminalCell>`. A render pass takes a shallow frame snapshot; mutating
the shared vector for a later dirty-row update can therefore detach and copy
the whole grid. A possible next step is row-sharded storage whose outer
container and individual row payloads are implicitly shared, so unchanged rows
remain shared and only dirty rows detach. This would trade flat traversal and
simple indexing for finer-grained copies, so it must first be justified by an
extension to `bench-terminal-frame-materialization`: retain a shallow render
snapshot while applying one-row, several-row, and full-frame updates, and
report apply time, allocations, and copied cell bytes at representative grid
sizes. Renderer benchmarks must also show that noncontiguous rows do not
regress scan and paint costs.

### Renderer

The retained renderer already avoids the largest terminal-frontend costs:

- ordinary terminal updates transport and rebuild dirty rows rather than the
  full grid;
- row dependency masks skip cell scans and shaping for unrelated global paint
  changes;
- RHI solid geometry, printable-ASCII glyph batches, and atlas resources are
  retained across compatible updates;
- Qt text nodes are created lazily for complex shaping and the software path;
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

## Building benchmarks

Benchmarks are opt-in and excluded from CTest:

```sh
cmake --preset release -DGHOSTTY_QT_BUILD_RENDER_BENCHMARKS=ON
cmake --build --preset release -j"$(nproc)"
```

| Executable | Measures |
| --- | --- |
| `bench-terminal-pane-renderer` | Full-frame, dirty-row, cursor, search, glyph-batch, and Kitty scene-graph work |
| `bench-terminal-custom-shader-compiler` | Cold shader baking and content-cache hits |
| `bench-terminal-custom-shader-rhi` | Retained versus legacy multi-pass shader recording and GPU work |
| `bench-terminal-kitty-graphics` | Kitty protocol replacement and isolated RGB/RGBA/grayscale materialization |
| `bench-terminal-frame-materialization` | Ghostty VT full-frame and dirty-row snapshots plus retained-frame application |
| `bench-terminal-search` | Visible-result latency, canonical history scan, cancellation, and recompression |
| `bench-terminal-backdrop` | Background asset preparation and software composition |

Representative invocations:

```sh
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

## I/O optimization roadmap

The current tests cover PTY correctness, final draining, and write
backpressure, but do not measure transport throughput or event-loop latency.
The next I/O stage must begin with a Linux-specific benchmark.

### 1. Add a PTY bridge benchmark

Add an opt-in `bench-terminal-session-io` that re-executes itself as a
deterministic child. Measure:

- output and input bytes per second;
- read/write syscall counts, bytes per call, and `EAGAIN` counts;
- calls and bytes submitted to libghostty;
- continuation activations and pending-write high-water mark;
- time to first and final frame;
- p50/p95/p99 delay for queued control requests during sustained output;
- copies, buffer compactions, and frame/update counts.

Workloads should include interactive one-byte input, 4 KiB chunks, a saturated
plain stream, ANSI-heavy output, query-heavy terminal replies, backpressured
large paste, and continuous output mixed with latency probes. Compare the same
corpus with direct `GhosttyVtAdapter::writeVt` and Ghostty's terminal-stream
benchmark to separate frontend transport from parser cost.

Use `perf stat` or `perf record -g` on Release builds to attribute CPU cost.
Use `strace` only for syscall topology because tracing perturbs latency.

### 2. Evaluate read batching

The worker currently calls libghostty after every successful PTY `read`.
Gathering repeated small nonblocking reads into one 64 KiB batch before parsing
could reduce C-API calls and drain the kernel sooner. Parse immediately on
`EAGAIN` so interactive output does not wait for a full buffer.

Compare byte quotas with a short elapsed-time budget and reject any change that
regresses p99 control/input latency.

Upstream Ghostty also has a separate gather/parser pipeline. Much of its stated
motivation concerns small macOS PTY reads; do not port the extra thread and
buffer ring until Linux measurements show that single-thread batching still
leaves meaningful kernel backpressure.

### 3. Remove avoidable write copies

Terminal-generated replies currently cross a borrowed libghostty callback into
an owning `QByteArray`, then copy again into `PtyWriteBuffer`.

Candidate changes:

- make the callback accept `QByteArrayView` for synchronous queuing;
- when the FIFO is empty, write directly from the caller's view;
- append only the unwritten suffix after `EAGAIN`;
- batch multiple replies only to the end of the current bounded parse call.

The benchmark must cover allocation/copy counts, query latency, ordering, and
backpressure before and after the change.

### 4. Reduce auxiliary process and IPC I/O

These are lower-frequency but concrete costs:

- cache successful identical shell-integration results with a bounded,
  process-wide, single-flight cache keyed by the serialized request plus every
  behavior-affecting helper, environment, and resource identity;
- compare the derived global-shortcut registry and avoid closing/recreating an
  unchanged active portal session;
- use exponential retry backoff for unchanged invalid configuration, reset by
  watcher or manual reload events.

Each uncached pane pays a helper process launch plus JSON/base64 pipe traffic,
and multiple panes commonly submit identical requests. Cache failures only if
their retry and invalidation semantics are explicitly defined.

### 5. Isolate cold filesystem work

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
