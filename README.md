# ghostty-qt

`ghostty-qt` is a work-in-progress Linux terminal emulator built with Qt Quick,
C++23, and Ghostty's `libghostty-vt` C API. Ghostty owns terminal parsing,
screen state, selection, and input encoding; Qt owns the Wayland windows,
controls, rendering, clipboard, and input-method integration.

The project targets Linux on Wayland only. It has a substantial usable feature
set, but it does not yet claim complete feature parity with the Ghostty
application.

## Highlights

- A real Linux PTY and shell or command per pane, with scrollback, true color,
  input methods, mouse reporting, bracketed paste, and terminal resizing.
- Qt scene-graph rendering with styled text, terminal decorations, cursor
  shapes, transparency, background images, and ordinary Kitty graphics
  placements.
- Tabs, nested splits, split zoom, directional navigation, draggable dividers,
  multiple windows, and a retained layer-shell quick terminal.
- Selection, search, clipboard integration, protected paste, OSC 52/1337
  clipboard writes, OSC 8 links, and default URL/path detection.
- Ghostty-compatible configuration, themes, palettes, keybindings, shell
  integration, terminfo, and delegated CLI utilities from a pinned Ghostty
  revision.
- Live configuration reload, dynamic light/dark appearance, command palette,
  pane titles, bell feedback, and Linux systemd cgroup integration.
- Standard desktop, D-Bus, and systemd user-service activation, including
  readiness/reload handshakes, `+new-window`, and `+toggle-quick-terminal`.

This list is intentionally not exhaustive. See [Project status](docs/status.md)
and the machine-checked [parity manifest](docs/ghostty-parity.json) for the
current compatibility boundary.

## Requirements

- Linux with a Wayland session and the Qt Wayland platform plugin.
- Qt 6.8 or newer with Core, D-Bus, Gui, Multimedia, Qml, Quick,
  Quick Controls 2, ShaderTools, Widgets, and Qt Test development components,
  including matching GuiPrivate and ShaderToolsPrivate headers.
- LayerShellQt.
- KF6 WindowSystem is optional; when present, it enables `background-blur` on
  KDE Plasma Wayland.
- KDE's `qqc2-desktop-style` is an optional runtime integration. On Plasma it
  makes Qt Quick Controls follow the active QWidget style, including Breeze
  and user-selected color schemes; Fusion remains the portable fallback.
- A C++23 compiler and standard library, CMake 3.24 or newer, and Ninja.
- Zig exactly 0.16.0.
- `pkg-config`, libxkbcommon and Fontconfig development files, Git, `patch`,
  Python 3.10 or newer, `tic`, Linux PTY headers, and `libutil`.

Cgroup isolation additionally needs a user systemd manager on the session
D-Bus. Its default soft-failure policy allows terminals to start when that
service is unavailable. Service readiness uses the stable `NOTIFY_SOCKET`
protocol directly and does not add a `libsystemd` build dependency. The
installed `Type=notify-reload` unit requires systemd 253 or newer; direct
launches remain independent of systemd.

## Quick start

Initialize the pinned Ghostty source and install the project-local Zig
toolchain:

```sh
git submodule update --init --recursive
./scripts/bootstrap-zig.sh
./.local/bin/zig version
```

The bootstrap script installs Zig under the ignored `.local` directory. It
downloads the official archive and verifies the published checksum.

Configure and build the developer preset:

```sh
cmake --preset dev
cmake --build --preset dev -j"$(nproc)"
```

The first build downloads Ghostty's Zig dependencies and builds a private
configuration helper, so it is much slower than an ordinary incremental build.

Run from a Wayland session:

```sh
./build/dev/ghostty-qt
```

To start a specific command, use Ghostty's `-e` spelling. Every remaining
argument belongs to the command:

```sh
./build/dev/ghostty-qt --working-directory="$PWD" -e /bin/bash -l
./build/dev/ghostty-qt --hold -e /bin/sh -c 'printf "done\n"'
```

The frontend's earlier `-- program ...` spelling remains supported.

For an optimized build, replace `dev` with `release`. Do not build different
presets concurrently in one checkout because the embedded Ghostty build shares
source-tree Zig output. More workflows and caveats are in
[Development and CI](docs/development.md).

## Configuration

ghostty-qt reads two independent configuration domains:

```text
$XDG_CONFIG_HOME/ghostty/config
$XDG_CONFIG_HOME/ghostty/config.ghostty
$XDG_CONFIG_HOME/ghostty-qt/config
```

The first two are standard Ghostty files for portable terminal behavior,
appearance, keybindings, and Linux settings. The final file contains the small
set of Qt-owned application settings. If `XDG_CONFIG_HOME` is unset or
relative, both domains use `$HOME/.config`.

See [Configuration](docs/configuration.md) for precedence, reload behavior,
examples, and the compatibility source of truth. The Qt-owned file's complete
grammar is documented in
[Frontend configuration](docs/frontend-configuration.md).

## Tests

After building the developer preset:

```sh
ctest --preset dev -j"$(nproc)" --output-on-failure
```

The sanitizer workflow, focused tests, renderer benchmark, formatting hook, and
headless smoke test are documented in
[Development and CI](docs/development.md).

## Documentation

- [Usage differences](docs/usage.md): command-line, configuration, and
  frontend behavior that differs from Ghostty.
- [Configuration](docs/configuration.md): shared Ghostty settings, Qt-owned
  settings, precedence, reload, and keybinding examples.
- [Installation and desktop integration](docs/installation.md): install-tree
  layout, D-Bus activation, terminfo, and shell resources.
- [Project status](docs/status.md): supported scope, notable gaps, and how
  parity is tracked.
- [Architecture](docs/architecture.md): process, PTY, renderer, configuration,
  keybinding, and application-lifetime design.
- [Development and CI](docs/development.md): toolchains, tests, sanitizers,
  benchmarks, formatting, and CI.
- [Feasibility and stack decision](docs/feasibility.md): why the project uses
  C++/Qt with a narrow C boundary to Ghostty.
- [Features requiring upstream Ghostty APIs](REQUIRES_UPSTREAM.md): exact
  blockers that should not be implemented by patching the pinned submodule.

The authoritative Ghostty commit is recorded in
[`GHOSTTY_REVISION`](GHOSTTY_REVISION). The official submodule is kept
unmodified; intentional revision upgrades update the pin, gitlink, integration
code, and parity ledger together.
