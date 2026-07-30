# Usage differences from Ghostty

ghostty-qt follows Ghostty's Linux behavior where the Qt frontend and available
APIs permit it. This page documents only the user-visible differences; it is
not a general terminal usage guide.

## Executable and command line

The executable is `ghostty-qt`. Pass an initial terminal command after `--`:

```sh
ghostty-qt --working-directory "$PWD" -- /bin/bash -l
```

Unlike Ghostty, `-e` is not a supported terminal-launch option. Ordinary
`--help` and `--version` describe the Qt frontend.

The configuration-enabled build delegates only this pinned subset of Ghostty
CLI actions before Qt starts:

```text
+edit-config     +explain-config  +help          +list-actions
+list-colors     +list-keybinds    +show-config   +ssh
+ssh-cache       +validate-config
```

It implements `+new-window` and `+toggle-quick-terminal` as D-Bus application
clients. Other upstream `+` actions fail explicitly instead of being treated as
terminal commands. Upstream catalogs printed by `+help` or `+list-actions` are
not claims that every listed frontend action is implemented.

## Configuration

Portable terminal settings and keybindings remain in the standard Ghostty
files. Qt-owned application policy instead uses:

```text
$XDG_CONFIG_HOME/ghostty-qt/config
```

The separate file owns `single-instance`, `tabs-location`, `wide-tabs`,
`horizontal-tab-scroll`, `quick-terminal-layer`, and
`quick-terminal-namespace`.
Ghostty's `gtk-*` configuration keys do not configure Qt equivalents.
`--gtk-single-instance` is accepted only as a hidden command-line migration
alias for `--single-instance`.

`open_config` and `+edit-config` continue to open the standard Ghostty
configuration, not the Qt-owned file. See
[Configuration](configuration.md) for paths and precedence.

## Frontend behavior

- Windows, controls, menus, dialogs, text shaping, and final rendering are
  Qt-owned rather than GTK/libadwaita-owned.
- The quick terminal is a LayerShellQt surface. It uses the portable
  quick-terminal settings plus Qt-owned layer and namespace settings from
  `ghostty-qt/config`; the GTK-prefixed spellings do not apply.
- Precision horizontal scrolling over a terminal switches tabs by default.
  Set `horizontal-tab-scroll = false` in `ghostty-qt/config` to forward it to
  the terminal instead; discrete horizontal wheel input is always forwarded.
- `background-blur` uses KWin's whole-window blur effect when ghostty-qt was
  built with KF6 WindowSystem. Other Wayland compositors ignore it, and KWin's
  protocol does not expose Ghostty's configured radius.
- Restoring decorations on an existing Wayland window may use Qt client-side
  framing even when the window initially had compositor-side decorations.
- Shifted-punctuation fallback matching is currently US-layout-oriented
  because `QKeyEvent` does not expose the compositor keymap's unmodified level.

## Terminal API gaps

The most visible differences caused by the current Ghostty API boundary are:

- Qt performs final font fallback and shaping; exact Ghostty fallback, box/icon
  sprites, and color emoji are unavailable.
- Kitty graphics supports ordinary placements, but not Unicode virtual
  placements.
- Search uses a cooperative scan over public terminal snapshots rather than
  Ghostty's private search thread.
- Terminal clipboard writes support `allow`, `deny`, and `ask`, but
  terminal-requested clipboard reads are unavailable.
- Styled VT/HTML selection copies, semantic prompt navigation, and semantic
  screen clearing remain incomplete.

The machine-checked [parity manifest](ghostty-parity.json) is authoritative for
individual settings and actions. See [Project status](status.md) and
[upstream API requirements](../REQUIRES_UPSTREAM.md) for detailed limitations.
