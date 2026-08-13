# Usage differences from Ghostty

ghostty-qt follows Ghostty's Linux behavior where the Qt frontend and public
APIs permit it. This page only describes user-visible differences.

## Executable and command line

The executable is `ghostty-qt`. Ghostty's `-e` command boundary is supported:

```sh
ghostty-qt --working-directory="$PWD" -e /bin/bash -l
```

The earlier `-- program ...` spelling remains available. Ordinary `--help` and
`--version` describe the Qt frontend.

Supported non-GUI `+` actions are delegated to the pinned Ghostty helper before
Qt starts. `+new-window`, `+new-tab`, and `+toggle-quick-terminal` are instead
Qt-owned D-Bus clients. Known but unsupported upstream actions fail explicitly.
The [parity manifest](ghostty-parity.json) is authoritative for frontend
support; the delegated `ghostty-qt +help` may list upstream actions the Qt
frontend does not implement.

## Configuration

Standard Ghostty files provide the reusable base. The
`$XDG_CONFIG_HOME/ghostty-qt/config` file accepts both ordinary Ghostty keys as
final overrides and Qt-owned application policy. Ghostty's `gtk-*` settings do
not configure Qt equivalents.

`open_config` opens the mixed ghostty-qt file. The delegated `+edit-config`
action retains Ghostty's standard-file behavior. See
[Configuration](configuration.md) and
[Frontend configuration](frontend-configuration.md).

`--gtk-single-instance` remains a hidden deprecated command-line alias for
`--single-instance`; the Ghostty configuration key is still not authoritative.

## Frontend behavior

- Windows, controls, menus, dialogs, shaping, and final rendering are Qt-owned.
  On Plasma, an installed KDE Qt Quick Controls desktop style is selected
  automatically unless `QT_QUICK_CONTROLS_STYLE` is explicitly set.
- The quick terminal is a LayerShellQt surface. Its layer and namespace are
  Qt-owned settings; GTK-prefixed spellings do not apply.
- Precision horizontal scrolling over a terminal changes tabs by default.
  Disable `horizontal-tab-scroll` to forward it to the terminal. Discrete
  horizontal wheel input is always forwarded as buttons 6/7.
- `background-blur` uses KWin's whole-window blur effect when built with KF6
  WindowSystem. Other compositors ignore it.
- `custom-shader` requires Qt Quick's OpenGL or Vulkan RHI backend. Unsupported
  backends render the terminal without the effect and publish a diagnostic.
- Qt-owned pane lifecycle shaders compose after the persistent custom-shader
  chain and drive pane, tab, and window creation/destruction through
  `iPaneTransition`; see
  [Frontend configuration](frontend-configuration.md#pane-lifecycle-shaders).
- Restoring decorations on an existing Wayland window may use Qt client-side
  framing even if the initial surface had compositor-side decorations.

## Known compatibility differences

The most visible remaining differences are exact Ghostty font shaping and
fallback, Kitty Unicode virtual placements, terminal-requested clipboard
reads, exact styled selection formatting, and semantic prompt operations.

[Project status](status.md) summarizes these gaps. The
[parity manifest](ghostty-parity.json) and
[upstream API requirements](../REQUIRES_UPSTREAM.md) are authoritative for
individual settings, actions, and blockers.
