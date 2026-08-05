# Installation and desktop integration

The build tree is directly runnable, but installing the release preset provides
relocatable helper binaries, desktop activation and AppStream metadata, a
scalable application icon, terminfo, and shell integration and theme resources.
Direct build-tree runs keep their configuration-specific Wayland identity, but
skip Qt 6.11 host-registry registration when the matching desktop entry is not
installed; ghostty-qt's own portal clients remain available.

## Install

Configure and build the release preset, then select an installation prefix:

```sh
cmake --preset release
cmake --build --preset release -j"$(nproc)"
cmake --install build/release --prefix "$HOME/.local"
```

The install contains:

- `ghostty-qt` and its private configuration helper;
- the private Ghostty runtime required by that helper;
- a desktop entry, D-Bus service, systemd user service, scalable icon, and
  AppStream metadata;
- the compiled `xterm-ghostty` terminfo entry;
- the pinned Ghostty theme bundle;
- staged Bash, Elvish, Fish, Nushell, and Zsh integration resources.

Relative GNU installation directories are finalized against the actual
`cmake --install --prefix` value. The complete prefix can be moved afterward
because runtime resources are resolved relative to the executable.

## Desktop activation

Configuration-specific metadata is installed under:

```text
${CMAKE_INSTALL_DATADIR}/applications/io.github.JingYenLoh.ghostty_qt.desktop
${CMAKE_INSTALL_DATADIR}/dbus-1/services/io.github.JingYenLoh.ghostty_qt.service
${CMAKE_INSTALL_DATADIR}/systemd/user/app-io.github.JingYenLoh.ghostty_qt.service
${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps/io.github.JingYenLoh.ghostty_qt.svg
${CMAKE_INSTALL_DATADIR}/metainfo/io.github.JingYenLoh.ghostty_qt.metainfo.xml
```

A Debug install appends `.Debug` to these filenames and the application
identity, so it neither activates nor overwrites a Release installation's
desktop assets.

The established application ID retains the mixed-case GitHub owner segment.
AppStream accepts it, although pedantic validation recommends lowercase-only
component IDs. Changing it requires a coordinated desktop and D-Bus identity
migration rather than a metadata-only rename.

The desktop entry names the installed executable through `TryExec`, requests
single-instance startup, and provides a New Window desktop action. Its
`X-TerminalArgExec`, `X-TerminalArgTitle`, `X-TerminalArgAppId`,
`X-TerminalArgDir`, and `X-TerminalArgHold` fields expose the supported `-e`,
`--title=`, `--class=`, `--working-directory=`, and
`--wait-after-command` spellings to desktop terminal launchers.

The D-Bus service delegates to the matching systemd user unit when the session
bus supports systemd activation, while retaining its direct `Exec` fallback.
The unit starts a resident zero-window host, waits for an exact startup
readiness notification, and maps `systemctl --user reload` to the application's
asynchronous configuration reload. `Type=notify-reload` and `ReloadSignal=`
require systemd 253 or newer; on other service managers the direct D-Bus
`Exec` fallback and ordinary direct launches remain available. Warm and cold
activation support ordinary
launches, `+new-window`, and `+toggle-quick-terminal`. Activation tokens and
startup IDs, D-Bus starter state, and systemd service identity are scoped to the
application and scrubbed before terminal children inherit their base
environment; an explicit Ghostty `env` entry may intentionally reintroduce a
value.

D-Bus activation does not require enabling the unit. After installing or
upgrading it, the user manager can be refreshed and the service inspected or
reloaded directly with:

```sh
systemctl --user daemon-reload
systemctl --user start app-io.github.JingYenLoh.ghostty_qt.service
systemctl --user reload app-io.github.JingYenLoh.ghostty_qt.service
```

Enabling the unit is optional and makes the zero-window host start with the
graphical session. Direct build-tree runs have no installed unit, but still
honor `NOTIFY_SOCKET` when launched by a compatible service manager.

The desktop entry, notifications, and AppStream component share the same icon
theme identity. The installed vector-only, font-free SVG is intentionally
simple enough to remain legible at 16 pixels. When available, the staged-install
test validates the desktop entry, AppStream document, XML, and rendered SVG
with the host's freedesktop.org tooling.

The repository does not currently declare a top-level software license, so the
AppStream component conservatively reports `LicenseRef-proprietary`; its
metadata document is separately reusable under CC0-1.0. Distribution-specific
packages remain future work.

## Terminfo

Every build generates Ghostty's `xterm-ghostty` entry under:

```text
build/<preset>/share/terminfo
```

Before finalized `env` overrides, terminal children receive:

```text
TERM=xterm-ghostty
TERMINFO=<the build or installed private terminfo directory>
COLORTERM=truecolor
```

A configured `term` changes `TERM` but does not generate another entry.
Configured `env` values may replace any injected variable.

Inspect the developer entry with:

```sh
infocmp -A build/dev/share/terminfo -x xterm-ghostty
```

An installed copy keeps its database under
`${CMAKE_INSTALL_DATADIR}/ghostty-qt/terminfo`. For diagnostics or a
nonstandard layout, `GHOSTTY_QT_TERMINFO` may point to a directory containing a
compiled `xterm-ghostty` entry. An invalid explicit override is an error rather
than a silent fallback.

## Theme resources

Every build stages Ghostty's pinned theme bundle under:

```text
build/<preset>/themes
```

An installed bundle lives under
`${CMAKE_INSTALL_DATADIR}/ghostty-qt/themes`. The `+list-themes` action finds
either location relative to its helper executable, so moving a complete
installation prefix preserves theme discovery. A non-empty
`GHOSTTY_RESOURCES_DIR` remains an authoritative upstream-compatible override.
User themes continue to live in `$XDG_CONFIG_HOME/ghostty/themes`.

Theme listing, path and color filtering, plain output, and the interactive TTY
selector are provided by the pinned Ghostty helper. Named bundled themes are
also finalized by the pinned configuration helper; Qt consumes the resulting
colors through its existing configuration protocol and continues to own
terminal rendering.

## Shell integration resources

The build stages the pinned Ghostty shell resources and applies a checked
downstream patch only to the executable name used by its SSH wrappers. The
Ghostty submodule itself remains unmodified.

Installed resources live under:

```text
${CMAKE_INSTALL_DATADIR}/ghostty-qt/shell-integration
```

`GHOSTTY_QT_SHELL_INTEGRATION_RESOURCES` can select another resource root for
diagnostics or a nonstandard layout. That root must contain the
`shell-integration` directory.

Shell integration activation and feature selection use the standard Ghostty
`shell-integration` and `shell-integration-features` settings. The process
boundary and relocation design are described in [Architecture](architecture.md).
