# Development and CI

The checked-in CMake presets are the supported developer entry points. Each
preset uses the project-local Zig executable at `.local/bin/zig` and builds in
its own directory:

| Preset | C++ toolchain | Purpose |
| --- | --- | --- |
| `dev` | Default compiler, Debug | Normal development and tests |
| `release` | Default compiler, Release | Optimized build and release-mode tests |
| `sanitize` | Clang, Debug | AddressSanitizer and UndefinedBehaviorSanitizer |

Bootstrap the exact Zig version required by the pinned Ghostty revision with:

```sh
./scripts/bootstrap-zig.sh
```

The script supports x86-64 and AArch64 Linux. It downloads Zig 0.15.2 from the
official Zig release directory, verifies the architecture-specific SHA-256
published in Zig's release index, extracts it under `.local/toolchains`, and
creates `.local/bin/zig`. A downloaded archive is cached under `.cache/zig` and
is verified again before extraction. `ZIG_DOWNLOAD_CACHE` can select another
cache directory and `ZIG_DOWNLOAD_BASE_URL` can select an HTTPS mirror; the
expected checksum cannot be overridden.

Configure, build, and test a preset as one sequence:

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

For a sanitizer run, install Clang and use:

```sh
cmake --preset sanitize
cmake --build --preset sanitize --target clean
cmake --build --preset sanitize --parallel
ctest --preset sanitize
```

The sanitizer preset instruments the project's C and C++ targets with ASan and
UBSan, enables frame pointers, and stops on undefined behavior. It also changes
the pinned Ghostty build from `ReleaseFast` to `ReleaseSafe`, retaining Zig's
safety checks. The Ghostty static library is not ASan/UBSan-instrumented. A
failure in upstream Zig code can still be visible at an instrumented API
boundary, but this preset does not provide complete sanitizer coverage inside
that library.

The clean step is required when another preset has already populated the shared
`ghostty/zig-out` directory. Ghostty's CMake wrapper does not encode Zig
optimization flags in that output path, so an existing `ReleaseFast` archive
would otherwise satisfy the sanitizer build without being regenerated as
`ReleaseSafe`. Likewise, clean the destination preset before switching back to
a different Zig configuration. This is another reason never to build the
presets concurrently in one checkout.

## Continuous integration

GitHub Actions runs Debug, Release, and Clang ASan+UBSan against the minimum Qt
6.8 line, currently pinned to Qt 6.8.3. A fourth Debug job checks compatibility
with the current Qt line, pinned to Qt 6.11.1. Every job has its own Ubuntu
24.04 checkout, including the pinned Ghostty submodule, so Ghostty's source-tree
`zig-out` directory is never shared between simultaneous configurations. CI
verifies the Qt and Zig versions and Ghostty commit before configuring, then
runs the matching CMake build and CTest presets.

Do not build multiple presets concurrently in the same checkout. Although the
CMake output directories are separate, the embedded Ghostty build writes to
`ghostty/zig-out` in the shared source tree.
