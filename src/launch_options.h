#pragma once

#include "frontend_config.h"
#include "ghostty_config_snapshot.h"
#include "ghostty_config_values.h"
#include "terminal_session_options.h"
#include "terminal_typography.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <chrono>
#include <expected>
#include <optional>

struct LaunchOptions {
    // Finalized terminal identity for future children. This launch-only value
    // never mutates the environment of an already-running pane.
    QByteArray term = QByteArrayLiteral("xterm-ghostty");
    // Finalized shared Linux cgroup settings for future terminal children.
    LinuxCgroupConfig linuxCgroup;
    // Startup arbitration fixes this process fact once. Reloading the
    // frontend's single-instance preference cannot reclassify a live process.
    bool processUsesSingleInstance = false;
    QString workingDirectory;
    // `inherit` preserves the process cwd and its logical PWD spelling. A
    // concrete CLI, config, or OSC-derived directory clears this bit.
    bool inheritWorkingDirectory = false;
    // Distinguishes the process default from an explicit Qt command-line
    // override, which takes precedence over Ghostty's shared config.
    bool workingDirectoryExplicit = false;
    TerminalTypography typography;
    // These bits distinguish parser defaults from an explicit command-line
    // override. The process helper receives those overrides before recursive
    // config files and finalization, then returns one authoritative typography
    // snapshot; the bits remain launch/forwarding metadata.
    bool fontFamilyExplicit = false;
    bool fontSizeExplicit = false;
    TerminalAppearance appearance;
    ScrollbackLimit scrollbackLimit;
    bool scrollbackLimitExplicit = false;
    // Qt presents Ghostty's viewport state as an overlay control. The policy
    // affects only that frontend affordance; scrolling remains available when
    // the control is hidden.
    ScrollbarPolicy scrollbar = ScrollbarPolicy::System;
    // BEL presentation is frontend-owned. The worker publishes the event,
    // while panes, tabs, and host windows apply these finalized features.
    BellFeatures bellFeatures;
    // Custom bell audio consumes Ghostty's finalized absolute path. The
    // optional bit preserves whether playback failures should be quiet.
    std::optional<GhosttyConfigPath> bellAudioPath;
    double bellAudioVolume = 0.5;
    ConfirmCloseMode confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    TerminalSelectionClipboardOptions selectionClipboard;
    // Finalized Unicode scalar values used by Ghostty's word-selection
    // gestures. U+0000 is a valid boundary and remains in the vector.
    QVector<quint32> selectionWordChars;
    // Ghostty finalizes the platform click-repeat interval in milliseconds.
    // Preserve the full u32 value so the worker can classify repeated clicks.
    quint32 clickRepeatIntervalMilliseconds = 500;
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
    WindowNewTabPosition windowNewTabPosition = WindowNewTabPosition::Current;
    // Controls the frontend tab-bar visibility without affecting terminal
    // session state.
    WindowShowTabBar windowShowTabBar = WindowShowTabBar::Auto;
    // Qt-owned placement for the stable window toolbar and tab strip.
    TabsLocation tabsLocation = TabsLocation::Top;
    // Ghostty's normalized top-level decoration preference. The Qt host maps
    // None exactly to a frameless window; ordinary decorated Qt windows retain
    // the Auto, Client, or Server preference even where QPA cannot distinguish
    // those protocol choices.
    WindowDecorationMode windowDecoration = WindowDecorationMode::Auto;
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
    std::optional<std::chrono::milliseconds> quitAfterLastWindowClosedDelay;
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
    // The GUI routes a right-click, but the worker resolves the configured
    // semantics against authoritative selection and link state.
    RightClickAction rightClickAction = RightClickAction::ContextMenu;
    // Middle-click is a GUI input policy. It intentionally stays outside the
    // worker-owned terminal session options.
    MiddleClickAction middleClickAction = MiddleClickAction::PrimaryPaste;
    // Gates application-requested mouse tracking without changing the
    // terminal's DEC mouse mode. The policy is surface-local at runtime.
    bool mouseReporting = true;
    // Controls whether Shift remains terminal-bound while a DEC mouse mode is
    // active. True/False are overridable by XTSHIFTESCAPE upstream; until the
    // public libghostty-vt API exposes that terminal flag, the frontend uses
    // their configured fallback. Always/Never capture routing is exact.
    MouseShiftCapture mouseShiftCapture = MouseShiftCapture::False;
    // Hides the pointer after terminal-bound text input until subsequent
    // pointer interaction reveals it again.
    bool mouseHideWhileTyping = false;
    // Transfers surface focus after real pointer motion while the host window
    // is already active.
    bool focusFollowsMouse = false;
    // Ghostty finalizes separate multipliers for smooth precision devices and
    // discrete wheel notches. The frontend applies the appropriate value when
    // translating each Qt wheel event.
    MouseScrollMultiplier mouseScrollMultiplier;
    // Enables Ghostty's built-in URL matcher. OSC 8 hyperlinks remain
    // available independently of this setting.
    bool linkUrl = true;
    // Controls whether matched link destinations are shown before activation.
    // Ghostty defaults to previews for both regex and OSC 8 links.
    LinkPreviewMode linkPreviews = LinkPreviewMode::Always;
    // The helper publishes Ghostty's finalized structured set. The tagged
    // text alternative is retained for focused action/keybinding injection in
    // tests; the unavailable alternative enables built-in emergency shortcuts
    // when no configuration backend exists.
    GhosttyKeybindSource keybindSource;
    bool hold = false;
    bool showHelp = false;
    bool showVersion = false;
    QStringList program;

    bool operator==(const LaunchOptions &) const = default;
};

// Program and hold form Ghostty's process-wide one-shot initial-session
// payload. Every ordinary pane keeps the remaining launch policy unchanged.
[[nodiscard]] LaunchOptions withoutInitialCommand(LaunchOptions options);

// Explicitly project the broad application/pane configuration onto the
// smaller value types allowed to cross the session-thread boundary.
TerminalSessionLaunchOptions
toTerminalSessionLaunchOptions(const LaunchOptions &options);
TerminalSessionRuntimeOptions
toTerminalSessionRuntimeOptions(const LaunchOptions &options);

// Project one complete, validated, CLI-aware Ghostty snapshot onto a launch
// request. A byte-valued Ghostty
// scrollback-limit is marked as Bytes and passed unchanged to the pinned
// libghostty max_scrollback field:
// despite that C field's legacy "lines" wording, the pinned implementation
// forwards it to PageList's byte-sized logical history cap. The legacy
// --scrollback-lines option is converted through the documented
// column-aware capacity estimate in scrollbackLimitInBytes().
LaunchOptions applyGhosttyConfigSnapshot(const LaunchOptions &base,
                                         const GhosttyConfigSnapshot &snapshot);
// Apply the independent Qt frontend generation after shared Ghostty settings.
// Explicit frontend CLI values stored in base remain authoritative.
LaunchOptions
applyFrontendConfigSnapshot(const LaunchOptions &base,
                            const FrontendConfigSnapshot &snapshot);
// Resolve both independently reloadable configuration domains from the
// immutable process command line. Null snapshots retain built-in/CLI values.
LaunchOptions
resolveLaunchOptions(const LaunchOptions &base,
                     const GhosttyConfigSnapshot *ghosttySnapshot,
                     const FrontendConfigSnapshot *frontendSnapshot);

// A live child and an active process are deliberately separate. For an
// interactive shell, the shell remains live at its prompt while only a
// foreground job is active. Explicitly launched programs are active for their
// entire lifetime.
bool shouldConfirmClose(ConfirmCloseMode mode, bool childIsRunning,
                        bool hasActiveProcess);

// The first argument is expected to be the application name, as it is in
// QCoreApplication::arguments().
[[nodiscard]] std::expected<LaunchOptions, QString>
parseLaunchOptions(const QStringList &arguments);

// Project the frontend's explicit font overrides back into Ghostty CLI
// spelling. The config helper receives these after its action so pinned
// Ghostty performs repeatable-family replacement and styled-face inheritance
// before exporting the finalized typography.
[[nodiscard]] QStringList
ghosttyConfigCliFontArguments(const LaunchOptions &options);

// Pure startup policy used before creating QML or terminal runtime objects.
[[nodiscard]] bool shouldUseSingleInstance(const LaunchOptions &options,
                                           QByteArrayView termProgram);
