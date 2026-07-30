#pragma once

#include "application_shell_options.h"
#include "frontend_config.h"
#include "ghostty_config_snapshot.h"
#include "ghostty_config_values.h"
#include "modifier_remap_types.h"
#include "terminal_initial_input.h"
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
    // Live raw response to a terminal ENQ control byte. Empty disables the
    // reply without disabling recognition of the control sequence.
    QByteArray enquiryResponse;
    // Ghostty's ordinary command applies to every new pane. Initial-command
    // remains separate because its process-wide lease may be won by any pane
    // whose terminal initialization reaches the coordinator first.
    std::optional<TerminalCommand> ordinaryCommand;
    std::optional<TerminalCommand> initialCommand;
    // Finalized launch input is sampled when a future pane is constructed.
    // Reloading never injects newly configured bytes into a running child.
    QVector<TerminalInitialInput> initialInput;
    // Live Linux threshold for retaining and presenting a quickly failed
    // child. The worker samples the newest value when it observes the exit.
    quint32 abnormalCommandExitRuntimeMilliseconds = 250;
    // Shared live wait policy. The CLI --hold bit below remains a distinct
    // initial-session override.
    bool waitAfterCommand = false;
    // Finalized raw environment overrides for future terminal children.
    TerminalEnvironment environment;
    // Automatic shell integration is resolved once for each future pane. A
    // reload changes newly launched shells without rewriting a live process.
    GhosttyShellIntegrationMode shellIntegration =
        GhosttyShellIntegrationMode::None;
    GhosttyShellIntegrationFeatures shellIntegrationFeatures;
    bool shellIntegrationAvailable = false;
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
    // Resolved process/window appearance and selected Ghostty conditional
    // state. All existing panes receive the latter through worker runtime
    // options; window chrome remains on the GUI thread.
    TerminalColorScheme colorScheme = TerminalColorScheme::Light;
    std::optional<QString> configuredTitle;
    // Startup-only application identity. The explicit bit distinguishes an
    // omitted CLI option from `--class=`, which clears the configured value
    // back to the frontend fallback.
    std::optional<QByteArray> applicationClass;
    bool applicationClassExplicit = false;
    WindowAppearanceOptions windowAppearance;
    // These bits distinguish parser defaults from an explicit command-line
    // override. The process helper receives those overrides before recursive
    // config files and finalization, then returns one authoritative typography
    // snapshot; the bits remain launch/forwarding metadata.
    bool fontFamilyExplicit = false;
    bool fontSizeExplicit = false;
    // This Ghostty CLI-only switch controls whether its standard config files
    // participate in the effective generation. Preserve both the value and
    // explicitness so the helper sees the original startup policy and
    // single-instance arbitration never drops an unforwardable override.
    bool configDefaultFiles = true;
    bool configDefaultFilesExplicit = false;
    TerminalAppearance appearance;
    // Qt-only compositing policy. This deliberately does not enter
    // TerminalSessionRuntimeOptions, so opacity reloads repaint existing panes
    // without waking SessionWorker or mutating libghostty terminal state.
    TerminalBackgroundOptions background;
    // Frontend-owned pinned Ghostty blur value. Keep its two negative glass
    // sentinels intact even though Linux treats either as an enabled blur.
    qint16 backgroundBlur = 0;
    // Padding dimensions are captured per pane; balance and color are
    // frontend-owned policies that can be repainted/relaid out live.
    TerminalPaddingOptions padding;
    ScrollbackLimit scrollbackLimit;
    bool scrollbackLimitExplicit = false;
    // Live per-screen Kitty image storage budget. Unlike scrollback, the
    // public libghostty API can resize this allocation for existing screens.
    quint32 kittyImageStorageLimitBytes = 320'000'000;
    // Compression is a live session policy. Disabling it leaves existing
    // compressed pages intact while preventing further background work.
    bool scrollbackCompression = true;
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
    // Live access policy for terminal-originated writes such as OSC 52.
    TerminalClipboardAccess clipboardWrite = TerminalClipboardAccess::Allow;
    TerminalClipboardPasteOptions clipboardPaste;
    // Live policy for returning a scrolled-back viewport to the active
    // terminal output after PTY-bound input or newly displayed output.
    TerminalScrollToBottomOptions scrollToBottom;
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
    // This transport accepts only source-less activation. Derive whether this
    // invocation carries an option/program that would need forwarding so the
    // answer cannot drift from the actual launch state.
    [[nodiscard]] bool hasUnforwardedLaunchPayload() const noexcept
    {
        return workingDirectoryExplicit || fontFamilyExplicit
            || fontSizeExplicit || applicationClassExplicit
            || configDefaultFilesExplicit || scrollbackLimitExplicit || hold
            || !program.isEmpty();
    }
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
    // Allows terminal-owned ANSI KAM mode 2 to suppress ordinary keyboard and
    // IME input. Keybindings and their raw actions remain independent.
    bool vtKamAllowed = false;
    // Enables Ghostty's built-in URL matcher. OSC 8 hyperlinks remain
    // available independently of this setting.
    bool linkUrl = true;
    // Controls whether matched link destinations are shown before activation.
    // Ghostty defaults to previews for both regex and OSC 8 links.
    LinkPreviewMode linkPreviews = LinkPreviewMode::Always;
    // Process/window UI state is grouped so application-only reloads can
    // update retained window controllers without waking terminal workers.
    ApplicationShellOptions applicationShell;
    // Finalized, ordered modifier rewrites are live input policy. Their
    // first-match ordering is significant for both root and pane input.
    QVector<ModifierRemap> modifierRemaps;
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

// Initial-command, positional program, and hold form Ghostty's process-wide
// one-shot initial-session payload. Every ordinary pane keeps command and the
// remaining launch policy unchanged.
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
// Passing by value lets callers transfer an already-resolved Ghostty snapshot
// without another full LaunchOptions copy. Explicit frontend CLI values stored
// in the value remain authoritative.
LaunchOptions
applyFrontendConfigSnapshot(LaunchOptions options,
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

// Project every explicit Ghostty configuration override back into Ghostty CLI
// spelling. The config helper receives these after its action so pinned
// Ghostty owns parsing, recursive-file loading, precedence, and finalization.
[[nodiscard]] QStringList
ghosttyConfigurationArguments(const LaunchOptions &options);

// Pure startup policy used before creating QML or terminal runtime objects.
[[nodiscard]] bool shouldUseSingleInstance(const LaunchOptions &options,
                                           QByteArrayView termProgram);
