# Frontend configuration

The application-specific configuration file is:

```text
$XDG_CONFIG_HOME/ghostty-qt/config
```

If `XDG_CONFIG_HOME` is unset or relative, the path is
`$HOME/.config/ghostty-qt/config`. This is a mixed final override: ordinary
Ghostty keys are accepted alongside the Qt-owned keys documented below. The
standard Ghostty files remain the reusable base; see
[Configuration](configuration.md). GTK-prefixed settings are not Qt aliases.

## Precedence

Ghostty-owned options use this precedence:

1. built-in defaults;
2. the standard Ghostty configuration and recursive includes;
3. Ghostty assignments in this file and their recursive includes;
4. explicit ghostty-qt command-line overrides.

Qt-owned options use built-in defaults, this file, then explicit frontend
command-line overrides.

Frontend `single-instance` owns Qt process arbitration; Ghostty's
`gtk-single-instance` does not participate. Reload cannot reclassify a running
process, while a fresh launch samples the newest value. An explicit
`--single-instance=false|true|detect` value has highest precedence.

## Mixed grammar

Each non-comment line is routed by key ownership. The ten Qt-owned keys below
use a strict UTF-8 scalar assignment:

```text
key = value
```

Leading and trailing whitespace around a Qt line, key, and value is ignored.
Unix and CRLF line endings and a UTF-8 byte-order mark are accepted. A comment
begins with `#` only when `#` is the first non-whitespace character on its
line.

For Qt-owned keys, the following are errors:

- invalid UTF-8 or control characters other than a tab;
- an assignment with zero or more than one `=`;
- an empty value;
- a duplicate key;
- an unsupported value.

Every other line is passed unchanged to the pinned Ghostty parser. This keeps
Ghostty's quoting, byte encoding, repeated keys, resets, embedded `=` values,
conditionals, and `config-file` behavior. A key unknown to both domains is
therefore reported as a Ghostty configuration diagnostic rather than a
frontend "unknown key" error.

The two domains retain independent last-known-good generations. A bad Qt value
does not discard the last valid Ghostty generation, and a bad Ghostty value
does not discard the last valid Qt generation. Assignment diagnostics identify
the original mixed file and line.

For example:

```text
# Qt application policy
single-instance = detect
tabs-location = bottom
wide-tabs = false
horizontal-tab-scroll = true
quick-terminal-layer = overlay
quick-terminal-namespace = ghostty-quick-terminal
pane-enter-transition-shader = shaders/crt-pane-transition.glsl
pane-exit-transition-shader = shaders/crt-pane-transition.glsl
pane-enter-transition-duration = 180ms
pane-exit-transition-duration = 240ms

# Final Ghostty overrides
font-size = 13
maximize = true
background-opacity = 0.95
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
| `pane-enter-transition-shader` | shader path | unset | Adds a pane-creation shader after the persistent `custom-shader` chain. Relative paths resolve against the containing configuration file; `~/` expands to the home directory. Outside an enter transition the generated stage is an exact pass-through. |
| `pane-exit-transition-shader` | shader path | unset | Adds a pane-destruction shader after the persistent `custom-shader` chain. Outside an exit transition the generated stage is an exact pass-through. |
| `pane-enter-transition-duration` | `0ms` through `10000ms` | `0ms` | Runs the enter shader for a finite pane-creation transition. For compatibility, when no dedicated enter shader is set, the persistent `custom-shader` chain can still consume `iPaneTransition`. Zero keeps immediate presentation. |
| `pane-exit-transition-duration` | `0ms` through `10000ms` | `0ms` | Defers pane, tab, or window destruction while visible terminal panes run the exit shader. For compatibility, an unset dedicated shader falls back to the persistent chain. Zero keeps immediate destruction. Unsupported backends, missing shaders, hidden panes, and shader failures fail open without delaying close. |

## Pane lifecycle shaders

Every custom shader receives `vec4 iPaneTransition`:

- `x` is normalized progress from 0 to 1;
- `y` is `1` while entering, `-1` while exiting, and `0` while stable;
- `z` is elapsed transition time in seconds; and
- `w` is the configured duration in seconds.

The generated shader preamble also defines `PANETRANSITION_ENTER`,
`PANETRANSITION_EXIT`, and `PANETRANSITION_STABLE`. Dedicated lifecycle shaders
are compiled with a phase gate: the enter shader is invoked only while entering
and the exit shader only while exiting. At all other times each stage copies
`iChannel0` unchanged. They are ordered after every persistent `custom-shader`,
so a persistent BetterCRT-style effect feeds the lifecycle effect. Lifecycle
frames run even when Ghostty's `custom-shader-animation` is `false`; after the
finite transition the ordinary animation policy applies again.

The source tree includes `examples/shaders/crt-pane-transition.glsl`, and an
installation places it under `share/ghostty-qt/shaders/`. It is a
lifecycle-only CRT collapse. Copy it to a stable config path and use:

```text
custom-shader = ~/.config/ghostty/better-crt.glsl
custom-shader-animation = false
pane-enter-transition-shader = ~/.config/ghostty-qt/crt-pane-transition.glsl
pane-exit-transition-shader = ~/.config/ghostty-qt/crt-pane-transition.glsl
pane-enter-transition-duration = 180ms
pane-exit-transition-duration = 240ms
```

Creation begins once asynchronous shader compilation and the first usable Qt
Quick render stage are ready. Closing retires the affected pane IDs and stops
their terminal workers immediately, but keeps their last rendered pixels until
the exit interval and one final-frame grace have completed. Tab and window
chrome remain outside the pane shader.

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
independently. `open_config` opens this mixed application file; the delegated
`+edit-config` action continues to open the standard Ghostty configuration.

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
