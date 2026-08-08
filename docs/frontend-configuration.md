# Frontend configuration

Qt-owned application policy has a small independent configuration file:

```text
$XDG_CONFIG_HOME/ghostty-qt/config
```

If `XDG_CONFIG_HOME` is unset or relative, the path is
`$HOME/.config/ghostty-qt/config`. Portable terminal settings remain in the
standard Ghostty files; see [Configuration](configuration.md). The two domains
have disjoint keys, and GTK-prefixed settings are not Qt aliases.

## Precedence

Effective options use this precedence:

1. built-in defaults;
2. the finalized shared Ghostty configuration;
3. the ghostty-qt frontend configuration for its disjoint, Qt-owned keys;
4. explicit ghostty-qt command-line overrides.

Frontend `single-instance` owns Qt process arbitration; Ghostty's
`gtk-single-instance` does not participate. Reload cannot reclassify a running
process, while a fresh launch samples the newest value. An explicit
`--single-instance=false|true|detect` value has highest precedence.

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
before the first window. It watches the file and nearest existing parent, so
creation, deletion, and atomic replacement are detected. Reload runs away from
the GUI thread after a short debounce.

The loader accepts only a regular file and reads at most 1 MiB. A missing file
successfully restores defaults. Syntax or I/O failure retains the last good
snapshot and opens source-labelled Retry/Ignore diagnostics; no partial
document is published.

`reload_config` requests both configuration domains, which publish
independently. `open_config` still opens the shared Ghostty file, so edit this
frontend file directly.

The pre-GUI `+new-window` and `+toggle-quick-terminal` clients contact the
application service directly and do not load the launcher's frontend file.

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
