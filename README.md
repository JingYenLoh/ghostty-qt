# ghostty-qt

`ghostty-qt` is a Linux/Wayland terminal emulator built with Qt Quick, C++23,
and Ghostty's public `libghostty-vt` C API. Ghostty owns terminal parsing,
screen state, selection, and input encoding; Qt owns the application, native
Wayland integration, rendering, and desktop UI.

The application is usable and has broad terminal, workspace, and desktop
coverage. Complete Ghostty frontend parity is not yet claimed; remaining
differences are tracked explicitly.

## Highlights

- Nonblocking Linux PTYs with scrollback, true color, input methods, mouse
  reporting, bracketed paste, and terminal resizing.
- Retained Qt scene-graph rendering with styled text, transparency, background
  images, custom shaders, and ordinary Kitty graphics placements.
- Tabs, recursive splits, multiple windows, search, links, clipboard
  integration, protected paste, and a layer-shell quick terminal.
- Ghostty-compatible configuration, themes, palettes, keybindings, shell
  integration, terminfo, and delegated CLI utilities from a pinned revision.
- KDE-oriented Qt controls plus desktop, D-Bus, portal, notification, and
  systemd user-service integration.

See [Project status](docs/status.md) and the machine-checked
[parity manifest](docs/ghostty-parity.json) for the current compatibility
boundary.

## Requirements

- Linux with a Wayland session and the Qt Wayland platform plugin.
- Qt 6.8 or newer with Core, D-Bus, Gui, Multimedia, Qml, Quick,
  Quick Controls 2, ShaderTools, Widgets, and Qt Test development components,
  including matching GuiPrivate and ShaderToolsPrivate headers.
- LayerShellQt.
- KF6 WindowSystem is optional and enables `background-blur` on KDE Plasma.
- KDE's `qqc2-desktop-style` is an optional runtime integration for controls
  matching the Plasma style.
- A C++23 compiler and standard library, CMake 3.24 or newer, Ninja, and Zig
  exactly 0.16.0.
- `pkg-config`, libwayland-client, libxkbcommon and Fontconfig development
  files, Git, `patch`, Python 3.10 or newer, `tic`, Linux PTY headers, and
  `libutil`.

Cgroup isolation additionally needs a user systemd manager. The default
soft-failure policy still starts terminals without one. Installed
`Type=notify-reload` service units require systemd 253 or newer; direct
launches do not depend on systemd.

## Quick start

Initialize Ghostty and install the project-local Zig toolchain:

```sh
git submodule update --init --recursive
./scripts/bootstrap-zig.sh
```

Configure, build, and run:

```sh
cmake --preset dev
cmake --build --preset dev -j"$(nproc)"
./build/dev/ghostty-qt
```

Start a specific command with Ghostty's `-e` boundary:

```sh
./build/dev/ghostty-qt --working-directory="$PWD" -e /bin/bash -l
./build/dev/ghostty-qt --hold -e /bin/sh -c 'printf "done\n"'
```

The earlier `-- program ...` spelling remains supported. Replace `dev` with
`release` for an optimized build. Do not build presets concurrently in one
checkout because the embedded Ghostty build shares source-tree Zig output.

## Configuration

Portable terminal behavior stays in the standard Ghostty configuration:

```text
$XDG_CONFIG_HOME/ghostty/config
$XDG_CONFIG_HOME/ghostty/config.ghostty
```

Qt-owned application policy lives separately in:

```text
$XDG_CONFIG_HOME/ghostty-qt/config
```

See [Configuration](docs/configuration.md) for ownership and precedence, and
[Frontend configuration](docs/frontend-configuration.md) for the Qt-specific
grammar.

## Tests

```sh
ctest --preset dev -j"$(nproc)" --output-on-failure
```

Sanitizers, focused tests, formatting, and CI are documented in
[Development and CI](docs/development.md).

## Documentation

- [Usage differences](docs/usage.md)
- [Configuration](docs/configuration.md)
- [Frontend configuration](docs/frontend-configuration.md)
- [Installation and desktop integration](docs/installation.md)
- [Project status](docs/status.md)
- [Architecture](docs/architecture.md)
- [Performance](docs/performance.md)
- [Development and CI](docs/development.md)
- [Stack decision](docs/feasibility.md)
- [Features requiring upstream Ghostty APIs](REQUIRES_UPSTREAM.md)

The authoritative Ghostty commit is recorded in
[`GHOSTTY_REVISION`](GHOSTTY_REVISION). The official submodule remains
unmodified; an intentional upgrade updates the pin, gitlink, integration code,
and parity ledger together.
