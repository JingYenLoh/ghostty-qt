# Configuration

ghostty-qt keeps ordinary Ghostty files reusable and provides one final mixed
override file for both portable terminal settings and behavior owned by the Qt
frontend. GTK-prefixed settings are not aliases for Qt-specific policy.

## Files and ownership

The standard Ghostty files are loaded in upstream order:

```text
$XDG_CONFIG_HOME/ghostty/config
$XDG_CONFIG_HOME/ghostty/config.ghostty
```

They provide the shared base configuration for terminal behavior, commands,
environment, appearance, keybindings, shell integration, quick-terminal
policy, and Linux settings.

The final application override is:

```text
$XDG_CONFIG_HOME/ghostty-qt/config
```

It accepts ordinary Ghostty keys as well as the small Qt-owned key set. Ghostty
assignments in this file are parsed by the pinned parser after the standard
files and their recursive includes, so they override the shared configuration.
Qt-owned keys, mixed-file grammar, and reload semantics are documented in
[Frontend configuration](frontend-configuration.md).

If `XDG_CONFIG_HOME` is unset or not absolute, both domains fall back to
`$HOME/.config`.

Do not put ghostty-qt-only keys in the standard Ghostty files. Conversely,
`gtk-*` settings in either location do not configure Qt-owned equivalents.

## Precedence

Ghostty-owned options are resolved in this order:

1. built-in defaults;
2. the standard Ghostty files and their recursive includes;
3. Ghostty assignments in `ghostty-qt/config` and their recursive includes;
4. explicit ghostty-qt command-line overrides.

Qt-owned options use built-in defaults, then `ghostty-qt/config`, then their
explicit command-line overrides.

Ghostty's own file precedence, `config-file` includes, value parsing, and
finalization come from the pinned Ghostty implementation. The Qt process
consumes a typed private projection rather than parsing the human-oriented
`+show-config` output.

## Shared Ghostty settings

The supported slice includes common settings across these areas:

| Area | Examples |
| --- | --- |
| Process and session | `command`, `initial-command`, `input`, `env`, `term`, `working-directory`, `wait-after-command` |
| Terminal history and input | `scrollback-limit-bytes`, `scrollback-limit-lines`, `scrollback-compression`, `mouse-reporting`, `vt-kam-allowed`, `enquiry-response` |
| Appearance | `theme`, `window-theme`, `palette`, `foreground`, `background`, cursor and selection colors, cell metrics |
| Window and layout | padding, initial size/state, tab placement and visibility, split inheritance and zoom policy |
| Background and images | opacity, per-cell opacity, background images, and Kitty image storage limits |
| Clipboard and selection | copy-on-select, trimming and clearing policy, protected paste, `clipboard-write` |
| Search and links | search colors, `link-url`, and `link-previews` |
| Bell | `bell-features`, `bell-audio-path`, and `bell-audio-volume` |
| Linux integration | cgroup mode, hard-failure policy, memory limit, and process limit |
| Quick terminal | position, screen, size, autohide, and keyboard interactivity |
| Actions | `keybind`, `key-remap`, and command-palette entries |

This table is only a guide. The [parity manifest](ghostty-parity.json) is the
authoritative per-key compatibility ledger.

For example:

```ini
font-family = JetBrains Mono
font-size = 12
theme = light:light-theme,dark:dark-theme
window-theme = auto

scrollback-limit-bytes = 50000000
scrollback-limit-lines = unlimited
clipboard-write = ask
link-previews = true

maximize = true

quick-terminal-position = top
quick-terminal-size = 40%,100%
quick-terminal-autohide = true
```

The two scrollback limits are independent; history is pruned when either finite
limit is reached. Reloading them affects new panes rather than reallocating
existing history.

Use the pinned CLI implementations to inspect or validate the standard
configuration:

```sh
ghostty-qt +validate-config
ghostty-qt +show-config
ghostty-qt +explain-config
ghostty-qt +edit-config
```

These delegated actions run before the mixed application override is attached
and therefore describe the standard pinned Ghostty configuration plus explicit
CLI arguments. Catalog presence alone does not imply frontend support.

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
Shortcuts portal. Registration depends on the portal backend and compositor
and fails nonfatally.

The [keybinding-action inventory](ghostty-parity.json) is the source of truth
for implemented actions.

## Reload and failures

Both domains are watched and parsed away from the GUI thread. Live settings
update existing state; launch and construction settings become defaults for
later processes or surfaces.

`reload_config` explicitly requests both domains. Each publishes
independently. A malformed or unreadable update retains the last valid snapshot
for that domain and opens source-labelled Retry/Ignore diagnostics in every
window; a missing file restores its defaults. Successful post-startup
generations enqueue the window-local reload toast when `app-notifications`
enables `config-reload`.

The parser/helper and runtime ownership boundaries are in
[Architecture](architecture.md).
