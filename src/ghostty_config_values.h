#pragma once

#include "terminal_session_options.h"
#include "terminal_typography.h"

#include <QByteArray>
#include <QColor>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <array>
#include <chrono>
#include <optional>
#include <variant>

enum class ConfirmCloseMode {
    Never,
    RunningProcesses,
    Always,
};

enum class LinkPreviewMode {
    Never,
    Always,
    Osc8,
};

enum class ScrollbarPolicy {
    System,
    Never,
};

struct BellFeatures {
    bool system = false;
    bool audio = false;
    bool attention = true;
    bool title = true;
    bool border = false;

    bool operator==(const BellFeatures &) const = default;
};

struct MouseScrollMultiplier {
    double precision = 1.0;
    double discrete = 3.0;

    bool operator==(const MouseScrollMultiplier &) const = default;
};

enum class MiddleClickAction {
    PrimaryPaste,
    Ignore,
};

enum class MouseShiftCapture {
    False,
    True,
    Always,
    Never,
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

enum class WindowDecorationMode {
    Auto,
    Client,
    Server,
    None,
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

enum class GhosttyFullscreenMode {
    Disabled,
    Enabled,
    NonNative,
    NonNativeVisibleMenu,
    NonNativePaddedNotch,
};

enum class GhosttyCellRelativeColor {
    Foreground,
    Background,
};

// A required Ghostty terminal color is either an exact RGB value or a
// cell-relative alias. Nullable fields wrap this type in std::optional, so an
// unset value cannot be confused with either relative alternative.
using GhosttyTerminalColor = std::variant<QColor, GhosttyCellRelativeColor>;

enum class GhosttyBoldBrightness {
    Bright,
};

using GhosttyBoldColor = std::variant<QColor, GhosttyBoldBrightness>;

struct GhosttyAppearanceConfig {
    QColor foreground;
    QColor background;
    // Effective defaults after Ghostty applies palette generation and
    // harmonious light-theme orientation in the config helper.
    std::array<QColor, 256> palette;

    std::optional<GhosttyTerminalColor> selectionForeground;
    std::optional<GhosttyTerminalColor> selectionBackground;
    GhosttyTerminalColor searchForeground;
    GhosttyTerminalColor searchBackground;
    GhosttyTerminalColor searchSelectedForeground;
    GhosttyTerminalColor searchSelectedBackground;

    std::optional<GhosttyTerminalColor> cursorColor;
    TerminalCursorStyle cursorStyle = TerminalCursorStyle::Block;
    std::optional<bool> cursorBlink;
    double cursorOpacity = 1.0;
    std::optional<GhosttyTerminalColor> cursorText;

    std::optional<GhosttyBoldColor> boldColor;
    double faintOpacity = 0.5;

    bool operator==(const GhosttyAppearanceConfig &) const = default;
};

struct GhosttyConfigFile {
    QString path;
    bool optional = false;

    bool operator==(const GhosttyConfigFile &) const = default;
};

// A path finalized by Ghostty relative to its declaring config source. The
// provenance bit controls whether an inaccessible optional resource is quiet.
struct GhosttyConfigPath {
    QString path;
    bool optional = false;

    bool operator==(const GhosttyConfigPath &) const = default;
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

// Complete, frontend-ready projection of the configuration values exported by
// the pinned Ghostty helper. Textual enum tags, nullable wire sentinels, and
// optional-file prefixes are converted at the JSON boundary. Successful
// production parsing overwrites every member; defaults exist only to keep the
// value type regular and must not be treated as a second copy of Ghostty's
// configuration defaults.
struct GhosttyConfigValues {
    // Ghostty finalizes an empty source value back to this non-empty default.
    QByteArray term = QByteArrayLiteral("xterm-ghostty");
    // Raw bytes written to the PTY when the terminal receives ENQ. The empty
    // default deliberately means that ENQ is silent.
    QByteArray enquiryResponse;
    // The ordinary command is finalized for every terminal surface. The
    // optional initial command replaces it only for the first surface that
    // successfully initializes.
    std::optional<TerminalCommand> ordinaryCommand;
    std::optional<TerminalCommand> initialCommand;
    bool waitAfterCommand = false;
    // Ghostty stores this threshold as an exact u32 millisecond count rather
    // than Config.Duration. It remains live policy for a running surface.
    quint32 abnormalCommandExitRuntimeMilliseconds = 250;
    // The helper has already applied repeat/reset/remove/overwrite and include
    // precedence. Preserve the finalized raw key/value bytes unchanged.
    TerminalEnvironment environment;
    GhosttyShellIntegrationMode shellIntegration =
        GhosttyShellIntegrationMode::Detect;
    GhosttyShellIntegrationFeatures shellIntegrationFeatures;
    // Linux transient-scope policy remains launch-only: config reloads affect
    // panes constructed afterward, never an already-running process tree.
    LinuxCgroupConfig linuxCgroup;
    // nullopt is Ghostty's semantic `inherit`; a value is an already-finalized
    // concrete path whose lexical spelling must be preserved.
    std::optional<QString> workingDirectoryPath;
    TerminalTypography typography;

    GhosttyAppearanceConfig appearance;
    SplitAppearance splitAppearance;

    bool splitInheritWorkingDirectory = false;
    bool splitPreserveZoom = false;
    bool tabInheritWorkingDirectory = false;
    bool windowInheritWorkingDirectory = false;
    bool windowInheritFontSize = false;
    WindowNewTabPosition windowNewTabPosition = WindowNewTabPosition::Current;
    WindowShowTabBar windowShowTabBar = WindowShowTabBar::Auto;
    WindowDecorationMode windowDecoration = WindowDecorationMode::Auto;
    quint32 windowWidth = 0;
    quint32 windowHeight = 0;
    bool maximize = false;
    GhosttyFullscreenMode fullscreen = GhosttyFullscreenMode::Disabled;
    ResizeOverlayOptions resizeOverlay;

    quint64 scrollbackLimitBytes = 0;
    bool scrollbackCompression = true;
    ScrollbarPolicy scrollbar = ScrollbarPolicy::System;
    BellFeatures bellFeatures;
    std::optional<GhosttyConfigPath> bellAudioPath;
    double bellAudioVolume = 0.5;
    ConfirmCloseMode confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    TerminalSelectionClipboardOptions selectionClipboard{
        .trimTrailingSpaces = false,
        .copyOnSelect = TerminalCopyOnSelectMode::Disabled,
        .clearOnTyping = false,
        .clearOnCopy = false,
    };
    QVector<quint32> selectionWordChars;
    quint32 clickRepeatIntervalMilliseconds = 500;
    TerminalClipboardAccess clipboardWrite = TerminalClipboardAccess::Allow;
    TerminalClipboardPasteOptions clipboardPaste{
        .protection = false,
        .bracketedSafe = false,
    };
    TerminalScrollToBottomOptions scrollToBottom;
    RightClickAction rightClickAction = RightClickAction::ContextMenu;
    MiddleClickAction middleClickAction = MiddleClickAction::PrimaryPaste;
    bool mouseReporting = false;
    MouseShiftCapture mouseShiftCapture = MouseShiftCapture::False;
    bool mouseHideWhileTyping = false;
    bool focusFollowsMouse = false;
    MouseScrollMultiplier mouseScrollMultiplier;
    bool linkUrl = false;
    LinkPreviewMode linkPreviews = LinkPreviewMode::Never;

    QVector<GhosttyConfigFile> configFiles;
    bool quitAfterLastWindowClosed = false;
    std::optional<std::chrono::milliseconds> quitAfterLastWindowClosedDelay;
    bool initialWindow = false;
    SingleInstanceMode singleInstanceMode = SingleInstanceMode::Detect;

    bool operator==(const GhosttyConfigValues &) const = default;
};
