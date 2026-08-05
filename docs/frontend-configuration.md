# Frontend configuration

ghostty-qt has a small configuration domain for behavior owned by the Qt
frontend. It is intentionally separate from the shared Ghostty configuration:

```text
$XDG_CONFIG_HOME/ghostty-qt/config
```

If `XDG_CONFIG_HOME` is unset or is not an absolute path, the file is
`$HOME/.config/ghostty-qt/config`.

The standard Ghostty files remain the source for portable terminal settings,
keybindings, and Linux settings shared with upstream Ghostty:

```text
$XDG_CONFIG_HOME/ghostty/config
$XDG_CONFIG_HOME/ghostty/config.ghostty
```

Do not place ghostty-qt frontend keys in either Ghostty file. The pinned
Ghostty parser correctly reports them as unknown. Conversely, GTK-prefixed
settings in a Ghostty file do not configure their Qt-owned equivalents.

## Precedence

Effective launch options are rebuilt from immutable process arguments whenever
either configuration domain reloads:

1. built-in defaults;
2. the finalized shared Ghostty configuration;
3. the ghostty-qt frontend configuration for its disjoint, Qt-owned keys;
4. explicit ghostty-qt command-line overrides.

The two files do not provide competing spellings for the same setting. In
particular, frontend `single-instance` owns Qt process arbitration;
Ghostty's `gtk-single-instance` does not participate. An explicit
`--single-instance=false|true|detect` command-line value has the highest
precedence. The shared `linux-cgroup=single-instance` policy consumes the
actual primary role established by that startup arbitration. Reloading this
frontend key cannot reclassify the running process; a fresh process samples the
new value.

## Grammar

The frontend file is a strict UTF-8 scalar format. A UTF-8 byte-order mark is
accepted at the beginning. Each non-empty line must contain exactly one
assignment:

```text
key = value
```

Leading and trailing whitespace around the line, key, and value is ignored.
Unix and CRLF line endings are accepted. A comment begins with `#` only when
`#` is the first non-whitespace character on its line.

The following are errors:

- invalid UTF-8 or control characters other than a tab;
- an assignment with zero or more than one `=`;
- an empty key or value;
- an unknown or duplicate key;
- an unsupported value.

Keys and values are case-sensitive. Quoting and inline comments are not part of
the grammar. A malformed document is rejected as a whole; successfully parsed
keys are never applied partially. Assignment diagnostics identify the file and
line; encoding and I/O diagnostics identify the file.

For example:

```text
# Qt application policy
single-instance = detect
tabs-location = bottom
wide-tabs = false
horizontal-tab-scroll = true
quick-terminal-layer = overlay
quick-terminal-namespace = ghostty-quick-terminal
```

## Keys

| Key | Values | Default | Behavior |
| --- | --- | --- | --- |
| `single-instance` | `false`, `true`, `detect` | `detect` | Controls whether an eligible source-less launch participates in the process-wide `org.freedesktop.Application` endpoint. `detect` enables arbitration unless the launching environment advertises `TERM_PROGRAM`. The decision for an already-running process is fixed at startup; a fresh launcher reads the latest file. |
| `tabs-location` | `top`, `bottom` | `top` | Places the stable Qt toolbar and tab strip above or below the terminal content. A successful reload moves existing windows and becomes the default for future windows without recreating panes or changing window size. |
| `wide-tabs` | `false`, `true` | `true` | When true, tabs have equal widths and fill the available tab strip; when false, each tab uses its compact content-based width. A successful reload resizes retained tabs without recreating panes. |
| `horizontal-tab-scroll` | `false`, `true` | `true` | When true, precision horizontal scrolling over a terminal surface accumulates toward a 120-pixel gesture and changes one tab per threshold (`left` selects the next tab and `right` the previous); incomplete gestures reset after 500 milliseconds. When false, precision horizontal input is forwarded to the terminal. Discrete horizontal wheel input is always forwarded as buttons 6/7. |
| `quick-terminal-layer` | `background`, `bottom`, `top`, `overlay` | `top` | Selects the Wayland layer for the LayerShellQt quick terminal. A successful reload changes the retained native layer surface when the compositor supports layer-shell protocol version 2 or newer. |
| `quick-terminal-namespace` | any non-empty scalar | `ghostty-quick-terminal` | Sets the layer-shell namespace, called `scope` by LayerShellQt. Wayland fixes it when a native layer surface is created, so reload stages the value for a subsequently created or recreated native surface and cannot rename the currently mapped one. |

## Reload and failure behavior

The frontend service loads synchronously before single-instance arbitration and
before the first window is created. It watches both the file and its nearest
existing parent directory, so creating, deleting, or atomically replacing the
file is detected. File-system bursts are debounced for 75 milliseconds and
watched reloads run on a dedicated worker.

Each generation opens the path once, validates that opened descriptor as a
regular file, and reads at most 1 MiB. FIFOs and other non-regular inputs are
rejected without blocking, and a file that grows past the limit while being
read fails transactionally instead of publishing a truncated snapshot.

The `reload_config` application action requests both the shared Ghostty reload
and the frontend reload. Each domain publishes independently, and the
application resolves the latest successful snapshot from each one.
`open_config` continues to open the shared Ghostty file; edit the frontend file
at the path above directly.

A missing frontend file is a successful load of built-in frontend defaults.
Deleting an existing file therefore restores those defaults. Syntax and I/O
failures retain the last successful frontend snapshot and are retried
periodically; at initial startup, where no last-good snapshot exists, built-in
and explicit command-line values remain active. Hard failures open
source-labelled Retry/Ignore diagnostics in every window. Successful
post-startup generations enqueue the window-local reload toast when the shared
`app-notifications` setting enables `config-reload`.

The pinned private JSON schema-v3 export carries the upstream
`gtk-single-instance` value for source fidelity, but the Qt launch-option
resolver deliberately ignores it. The decoder accepts schema v3 only; this is
not a backwards-compatibility branch for an older frontend schema.

The pre-GUI `+new-window` and `+toggle-quick-terminal` clients do not load the
launching process's frontend file or consult its `single-instance` policy.
They contact a D-Bus service through `org.gtk.Actions`, allowing an existing
primary or the installed zero-window service host to handle the request with
its latest live window and quick-terminal policy. `+new-window --class=...`
selects a custom service identity; pinned `+toggle-quick-terminal` ignores
`--class` and always uses the build identity. Neither client depends on the
config-enabled Ghostty helper.

## Command-line migration

Use the Qt-owned option for desktop and direct launches:

```sh
ghostty-qt --single-instance=true
ghostty-qt --single-instance=true --initial-window=false
```

`--gtk-single-instance` remains a hidden deprecated command-line alias for
migration. It cannot be combined with `--single-instance`, and it does not make
the Ghostty `gtk-single-instance` configuration key authoritative for the Qt
frontend.
