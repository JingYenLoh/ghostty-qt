# Installation and desktop integration

The build tree is directly runnable. Installing a Release build adds runtime
helpers and resources, desktop activation, metadata, terminfo, themes, and
shell integration.

## Local distribution packages

Checkout-local convenience recipes are available for three package families:

- `dist/arch/PKGBUILD` produces an Arch package with `makepkg`;
- `dist/debian/build.sh` produces a `.deb` on Debian unstable; and
- `dist/rpm/build.sh` produces an RPM on current Fedora.

Initialize the submodule and project-local Zig toolchain before using any
recipe:

```sh
git submodule update --init --recursive
./scripts/bootstrap-zig.sh
```

On Arch, build and install with:

```sh
cd dist/arch
makepkg -si
```

On Debian unstable, install the build dependencies declared in
`dist/debian/control`, then build and install with:

```sh
./dist/debian/build.sh
sudo apt install ./dist/debian/ghostty-qt-local.deb
```

On Fedora, install `rpm-build` and the spec's build dependencies, then build
and install with:

```sh
sudo dnf install rpm-build
sudo dnf builddep dist/rpm/ghostty-qt.spec
./dist/rpm/build.sh
sudo dnf install dist/rpm/build/RPMS/*/ghostty-qt-*.rpm
```

These recipes package the current working tree and consume `.local/bin/zig`;
they do not fetch an immutable source archive. They are intended only for
local installation, build testing, and packaging iteration. They are not
signed release artifacts, do not carry distro release/upgrade policy, and are
not ready to submit to the AUR, Debian/Ubuntu archives, Fedora, or another
upstream package repository. Dependency names track the stated rolling/current
targets; older distro releases without Qt 6.10 cannot build ghostty-qt.

## Install

```sh
cmake --preset release
cmake --build --preset release -j"$(nproc)"
cmake --install build/release --prefix "$HOME/.local"
```

The installation contains:

- `ghostty-qt` and its private configuration helper/runtime;
- desktop, D-Bus, systemd user-service, icon, and AppStream files;
- the compiled `xterm-ghostty` terminfo entry;
- pinned Ghostty themes and shell-integration resources.

Relative GNU installation directories are finalized against the selected
prefix. The executable/helper/resource layout is relocatable because runtime
resources are resolved relative to the executable. Desktop, D-Bus, and systemd
metadata embeds the install-time executable path and must be regenerated after
moving an installation.

## KDE controls

Install KDE's Qt Quick Controls desktop style (`qqc2-desktop-style` on common
distributions) for controls that follow Plasma's active QWidget style.
ghostty-qt selects `org.kde.desktop` only when:

- the session identifies itself as KDE or Plasma;
- the QML module is discoverable; and
- `QT_QUICK_CONTROLS_STYLE` is not explicitly set.

This is an optional runtime integration, not a build dependency. Kirigami is
not required, and Qt's ordinary Linux style remains the fallback.

## Desktop activation

Release metadata is installed under the application ID
`io.github.JingYenLoh.ghostty_qt`. Here `<datadir>` means the configured GNU
data directory, normally `<prefix>/share`:

```text
<datadir>/applications/io.github.JingYenLoh.ghostty_qt.desktop
<datadir>/dbus-1/services/io.github.JingYenLoh.ghostty_qt.service
<datadir>/systemd/user/app-io.github.JingYenLoh.ghostty_qt.service
<datadir>/icons/hicolor/scalable/apps/io.github.JingYenLoh.ghostty_qt.svg
<datadir>/metainfo/io.github.JingYenLoh.ghostty_qt.metainfo.xml
```

Debug installs use a distinct `.Debug` identity and do not overwrite or
activate the Release application.

The desktop entry supports ordinary launch and New Window activation, and
advertises the terminal argument mappings for command, title, application ID,
working directory, and hold behavior.

The D-Bus service can delegate cold activation to the matching systemd user
unit while retaining a direct executable fallback. The unit starts a
zero-window service host and supports asynchronous reload. Its
`Type=notify-reload` contract requires systemd 253 or newer; direct launches
and D-Bus's direct fallback remain usable elsewhere.

After installation or upgrade:

```sh
systemctl --user daemon-reload
systemctl --user start app-io.github.JingYenLoh.ghostty_qt.service
systemctl --user reload app-io.github.JingYenLoh.ghostty_qt.service
```

Enabling the unit is optional and starts the resident host with the graphical
session. D-Bus activation itself does not require the unit to be enabled.

Direct build-tree runs have no matching installed desktop metadata. On Qt
versions that register the desktop identity with the host portal, ghostty-qt
skips only that registration when metadata is undiscoverable; its explicit
portal clients remain available.

## Terminfo

Every build generates:

```text
build/<preset>/share/terminfo
```

Before explicit `env` overrides, terminal children receive:

```text
TERM=xterm-ghostty
TERMINFO=<build or installed private terminfo directory>
COLORTERM=truecolor
```

Inspect the developer entry with:

```sh
infocmp -A build/dev/share/terminfo -x xterm-ghostty
```

Installed terminfo lives under `<datadir>/ghostty-qt/terminfo`. For diagnostics,
`GHOSTTY_QT_TERMINFO` can select another directory containing a compiled
`xterm-ghostty` entry. An invalid explicit override is an error, not a fallback.

## Themes

Builds stage pinned themes under `build/<preset>/themes`; installations use
`<datadir>/ghostty-qt/themes`. `+list-themes` finds either location relative
to the helper executable. `GHOSTTY_RESOURCES_DIR` is an authoritative
upstream-compatible override, and user themes remain under
`$XDG_CONFIG_HOME/ghostty/themes`.

Theme listing and configuration finalization come from the pinned Ghostty
helper; Qt consumes the resulting appearance values.

## Shell integration

Pinned Bash, Elvish, Fish, Nushell, and Zsh resources are installed under:

```text
<datadir>/ghostty-qt/shell-integration
```

The build applies a checked downstream executable-name patch to the staged SSH
wrappers without modifying the Ghostty submodule.

`GHOSTTY_QT_SHELL_INTEGRATION_RESOURCES` can select another resource root for
diagnostics. The root must contain a `shell-integration` directory. Shell
activation and feature selection use Ghostty's standard `shell-integration`
and `shell-integration-features` settings.
