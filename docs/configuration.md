# Configuration

ghostty-qt separates portable terminal configuration from behavior owned by
the Qt frontend. This keeps ordinary Ghostty files reusable while avoiding
GTK-prefixed compatibility aliases for Qt-specific policy.

## Files and ownership

The standard Ghostty files are loaded in upstream order:

```text
$XDG_CONFIG_HOME/ghostty/config
$XDG_CONFIG_HOME/ghostty/config.ghostty
```

They own terminal behavior, commands, environment, appearance, keybindings,
shell integration, quick-terminal policy, and Linux settings shared with
Ghostty.

Qt-owned settings live in:

```text
$XDG_CONFIG_HOME/ghostty-qt/config
```

That file owns `single-instance`, `tabs-location`, `quick-terminal-layer`, and
`quick-terminal-namespace`. Its strict grammar and complete semantics are
documented in
[Frontend configuration](frontend-configuration.md).

If `XDG_CONFIG_HOME` is unset or not absolute, both domains fall back to
`$HOME/.config`.

Do not put ghostty-qt keys in the standard Ghostty files. Conversely,
`gtk-*` settings do not configure Qt-owned equivalents.

## Precedence

Effective options are resolved in this order:

1. built-in defaults;
2. the finalized shared Ghostty configuration;
3. the disjoint ghostty-qt frontend configuration;
4. explicit ghostty-qt command-line overrides.

Ghostty's own file precedence, `config-file` includes, value parsing, and
finalization come from the pinned Ghostty implementation. The Qt process
consumes a typed private projection rather than parsing the human-oriented
`+show-config` output.

## Shared Ghostty settings

The supported slice includes common settings across these areas:

| Area | Examples |
| --- | --- |
| Process and session | `command`, `initial-command`, `input`, `env`, `term`, `working-directory`, `wait-after-command` |
| Terminal history and input | `scrollback-limit`, `scrollback-compression`, `mouse-reporting`, `vt-kam-allowed`, `enquiry-response` |
| Appearance | `theme`, `window-theme`, `palette`, `foreground`, `background`, cursor and selection colors, cell metrics |
| Window and layout | padding, initial size/state, tab placement and visibility, split inheritance and zoom policy |
| Background and images | opacity, per-cell opacity, background images, and Kitty image storage limits |
| Clipboard and selection | copy-on-select, trimming and clearing policy, protected paste, `clipboard-write` |
| Search and links | search colors, `link-url`, and `link-previews` |
| Bell | `bell-features`, `bell-audio-path`, and `bell-audio-volume` |
| Linux integration | cgroup mode, hard-failure policy, memory limit, and process limit |
| Quick terminal | position, screen, size, autohide, and keyboard interactivity |
| Actions | `keybind`, `key-remap`, and command-palette entries |

This table is a guide, not a compatibility ledger. The
[parity manifest](ghostty-parity.json) is authoritative for every upstream
configuration key and records whether its behavior is supported, partial,
planned, blocked by an upstream API, or outside this frontend's scope.

For example:

```ini
font-family = JetBrains Mono
font-size = 12
theme = light:light-theme,dark:dark-theme
window-theme = auto

scrollback-limit = 10000000
clipboard-write = ask
link-previews = true

quick-terminal-position = top
quick-terminal-size = 40%,100%
quick-terminal-autohide = true
```

Use the pinned CLI implementations to inspect or validate the standard
configuration:

```sh
ghostty-qt +validate-config
ghostty-qt +show-config
ghostty-qt +explain-config
ghostty-qt +edit-config
```

These actions run before Qt starts. Their output describes the pinned Ghostty
configuration; an item appearing in an upstream catalog does not by itself
mean that ghostty-qt implements its frontend behavior.

## Keybindings

Bindings use Ghostty's normal syntax. Root bindings, sequences, action chains,
catch-all triggers, named tables, and supported local, `all:`, and `global:`
dispatch are accepted.

```ini
keybind = ctrl+shift+m=activate_key_table:modal
keybind = modal/r=reload_config
keybind = modal/escape=deactivate_key_table

keybind = all:ctrl+shift+f=increase_font_size:1
keybind = global:super+shift+r=reload_config
keybind = global:super+grave=toggle_quick_terminal
```

Eligible direct root `global:` bindings register through the XDG Global
Shortcuts portal. Registration depends on the compositor or desktop and fails
nonfatally; there is no focus-only shortcut fallback.

The [keybinding-action inventory](ghostty-parity.json) is the source of truth
for implemented actions.

## Reload and failures

Both configuration domains are watched. Changes are debounced and parsed away
from the GUI thread; successful snapshots apply to existing state where the
setting has live semantics and become defaults for newly created surfaces.
Settings that inherently describe process startup or surface construction take
effect only for later processes, windows, tabs, or splits.

`reload_config` explicitly requests both domains. Each publishes
independently. A malformed or unreadable update retains the last valid snapshot
for that domain, and a missing file restores its defaults.

The detailed parser/helper boundary and per-setting ownership rules are in
[Architecture](architecture.md).
