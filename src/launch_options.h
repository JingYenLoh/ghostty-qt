#pragma once

#include "ghostty_config_snapshot.h"
#include "terminal_session_options.h"

#include <QColor>
#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <chrono>
#include <expected>
#include <optional>

enum class ConfirmCloseMode {
    Never,
    RunningProcesses,
    Always,
};

// Mirrors Ghostty's link-previews configuration without leaking its canonical
// text representation into the terminal UI.
enum class LinkPreviewMode {
    Never,
    Always,
    Osc8,
};

enum class MiddleClickAction {
    PrimaryPaste,
    Ignore,
};

enum class WindowNewTabPosition {
    Current,
    End,
};

enum class WindowShowTabBar {
    Always,
    Auto,
    Never,
};

enum class ResizeOverlayMode {
    Always,
    Never,
    AfterFirst,
};

enum class ResizeOverlayPosition {
    Center,
    TopLeft,
    TopCenter,
    TopRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

struct ResizeOverlayOptions {
    ResizeOverlayMode mode = ResizeOverlayMode::AfterFirst;
    ResizeOverlayPosition position = ResizeOverlayPosition::Center;
    std::chrono::milliseconds duration{750};

    bool operator==(const ResizeOverlayOptions &) const = default;
};

enum class SingleInstanceMode {
    Detect,
    Enabled,
    Disabled,
};

// Ghostty dims an unfocused pane by compositing this fill over the terminal.
// A missing fill resolves to the configured terminal background in the
// frontend; opacity describes the terminal content that remains visible.
struct SplitAppearance {
    double unfocusedOpacity = 0.7;
    std::optional<QColor> unfocusedFill;
    // An unset divider preserves the Qt frontend's ordinary reserved gap.
    std::optional<QColor> dividerColor;

    bool operator==(const SplitAppearance &) const = default;
};

struct LaunchOptions {
    QString workingDirectory;
    // `inherit` preserves the process cwd and its logical PWD spelling. A
    // concrete CLI, config, or OSC-derived directory clears this bit.
    bool inheritWorkingDirectory = false;
    // Distinguishes the process default from an explicit Qt command-line
    // override, which takes precedence over Ghostty's shared config.
    bool workingDirectoryExplicit = false;
    QString fontFamily;
    double fontSize = 12.0;
    // These bits distinguish parser defaults from an explicit command-line
    // override. Ghostty configuration must not replace explicit CLI fonts.
    bool fontFamilyExplicit = false;
    bool fontSizeExplicit = false;
    TerminalAppearance appearance;
    ScrollbackLimit scrollbackLimit;
    bool scrollbackLimitExplicit = false;
    ConfirmCloseMode confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    TerminalSelectionClipboardOptions selectionClipboard;
    TerminalClipboardPasteOptions clipboardPaste;
    // Split appearance belongs to the Qt pane/workspace renderers and never
    // crosses the terminal session-thread boundary.
    SplitAppearance splitAppearance;
    // New splits inherit the source pane's reported directory when enabled;
    // otherwise the workspace's effective working-directory is used.
    bool splitInheritWorkingDirectory = true;
    // When enabled, successful split navigation transfers an existing zoom
    // to the destination pane instead of restoring the full split tree.
    bool splitPreserveZoomNavigation = false;
    // New tabs inherit the source pane's reported directory when enabled;
    // otherwise the workspace's effective working-directory is used.
    bool tabInheritWorkingDirectory = true;
    // New windows inherit the originating/focused pane's reported directory
    // when enabled. With no live source, the application working-directory is
    // retained as the fallback.
    bool windowInheritWorkingDirectory = true;
    // Ghostty uses one setting for new windows and tabs. The current
    // frontend applies it to the source pane of a new tab or window.
    bool windowInheritFontSize = true;
    // Controls whether a new tab is inserted after the selected tab or at the
    // end of the window's stable tab list.
    WindowNewTabPosition windowNewTabPosition =
        WindowNewTabPosition::Current;
    // Controls the frontend tab-bar visibility without affecting terminal
    // session state.
    WindowShowTabBar windowShowTabBar = WindowShowTabBar::Auto;
    // Requested terminal grid for newly created windows. Zero retains the
    // frontend's ordinary sizing; presentation policy requires both values
    // to be nonzero before it derives a pixel geometry.
    quint32 windowWidth = 0;
    quint32 windowHeight = 0;
    // Transient pane-local grid feedback remains entirely on the GUI thread.
    ResizeOverlayOptions resizeOverlay;
    // Initial state for each newly created Qt window. These remain frontend
    // policy and never cross the terminal session-thread boundary.
    bool maximize = false;
    bool fullscreen = false;
    // Linux process lifetime is application-owned. Closing the final primary
    // window either quits on the next event turn, waits for the optional
    // Ghostty duration, or leaves the process alive when disabled.
    bool quitAfterLastWindowClosed = true;
    std::optional<std::chrono::milliseconds>
        quitAfterLastWindowClosedDelay;
    // Whether this process requests a window during initial application
    // activation. Explicit new-window actions remain available when false.
    bool initialWindow = true;
    // Command-line service bootstrap values must outrank the user's config.
    bool initialWindowExplicit = false;
    // This transport accepts only source-less activation. The parser marks
    // every option/program that would need forwarding; the exact coordination
    // flags remain payload-free.
    bool hasUnforwardedLaunchPayload = false;
    // Detect additionally excludes launches from a terminal advertising
    // TERM_PROGRAM. The structured helper preserves Ghostty's raw mode so the
    // originating process can resolve detect from its real invocation.
    SingleInstanceMode singleInstanceMode = SingleInstanceMode::Detect;
    bool singleInstanceModeExplicit = false;
    // Middle-click is a GUI input policy. It intentionally stays outside the
    // worker-owned terminal session options.
    MiddleClickAction middleClickAction = MiddleClickAction::PrimaryPaste;
    // Gates application-requested mouse tracking without changing the
    // terminal's DEC mouse mode. The policy is surface-local at runtime.
    bool mouseReporting = true;
    // Enables Ghostty's built-in URL matcher. OSC 8 hyperlinks remain
    // available independently of this setting.
    bool linkUrl = true;
    // Controls whether matched link destinations are shown before activation.
    // Ghostty defaults to previews for both regex and OSC 8 links.
    LinkPreviewMode linkPreviews = LinkPreviewMode::Always;
    // The helper publishes Ghostty's finalized structured sets. The explicit
    // bit distinguishes an intentionally empty root from the built-in
    // emergency shortcuts used when configuration is unavailable.
    GhosttyKeybindConfig keybindConfig;
    // Text fallback used by config-disabled builds and direct compatibility
    // tests. A structured snapshot clears this list before reaching a pane.
    QStringList keybindings;
    bool keybindingsConfigured = false;
    bool hold = false;
    bool showHelp = false;
    bool showVersion = false;
    QStringList program;

    bool operator==(const LaunchOptions &) const = default;
};

// Explicitly project the broad application/pane configuration onto the
// smaller value types allowed to cross the session-thread boundary.
TerminalSessionLaunchOptions toTerminalSessionLaunchOptions(
    const LaunchOptions &options);
TerminalSessionRuntimeOptions toTerminalSessionRuntimeOptions(
    const LaunchOptions &options);

// Overlay the compatibility slice of an available Ghostty snapshot onto a
// launch request. The function has no side effects and ignores malformed or
// unavailable values. A byte-valued Ghostty scrollback-limit is marked as
// Bytes and passed unchanged to the pinned libghostty max_scrollback field:
// despite that C field's legacy "lines" wording, the pinned implementation
// forwards it to PageList's byte-sized logical history cap. The legacy
// --scrollback-lines option is converted through the documented
// column-aware capacity estimate in scrollbackLimitInBytes().
LaunchOptions applyGhosttyConfigSnapshot(const LaunchOptions &base,
                                         const GhosttyConfigSnapshot &snapshot);

// A live child and an active process are deliberately separate. For an
// interactive shell, the shell remains live at its prompt while only a
// foreground job is active. Explicitly launched programs are active for their
// entire lifetime.
bool shouldConfirmClose(ConfirmCloseMode mode, bool childIsRunning,
                        bool hasActiveProcess);

// The first argument is expected to be the application name, as it is in
// QCoreApplication::arguments().
[[nodiscard]] std::expected<LaunchOptions, QString> parseLaunchOptions(
    const QStringList &arguments);

// Pure startup policy used before creating QML or terminal runtime objects.
[[nodiscard]] bool shouldUseSingleInstance(
    const LaunchOptions &options, QByteArrayView termProgram);
