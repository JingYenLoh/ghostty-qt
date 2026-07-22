#pragma once

#include "terminal_session_options.h"

#include <QColor>
#include <QString>
#include <QStringList>
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
    // nullopt is Ghostty's semantic `inherit`; a value is an already-finalized
    // concrete path whose lexical spelling must be preserved.
    std::optional<QString> workingDirectoryPath;
    QStringList fontFamilies;
    double fontSize = 0.0;

    GhosttyAppearanceConfig appearance;
    SplitAppearance splitAppearance;

    bool splitInheritWorkingDirectory = false;
    bool splitPreserveZoom = false;
    bool tabInheritWorkingDirectory = false;
    bool windowInheritWorkingDirectory = false;
    bool windowInheritFontSize = false;
    WindowNewTabPosition windowNewTabPosition = WindowNewTabPosition::Current;
    WindowShowTabBar windowShowTabBar = WindowShowTabBar::Auto;
    quint32 windowWidth = 0;
    quint32 windowHeight = 0;
    bool maximize = false;
    GhosttyFullscreenMode fullscreen = GhosttyFullscreenMode::Disabled;
    ResizeOverlayOptions resizeOverlay;

    quint64 scrollbackLimitBytes = 0;
    ConfirmCloseMode confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    TerminalSelectionClipboardOptions selectionClipboard{
        .trimTrailingSpaces = false,
        .copyOnSelect = TerminalCopyOnSelectMode::Disabled,
        .clearOnTyping = false,
        .clearOnCopy = false,
    };
    TerminalClipboardPasteOptions clipboardPaste{
        .protection = false,
        .bracketedSafe = false,
    };
    MiddleClickAction middleClickAction = MiddleClickAction::PrimaryPaste;
    bool mouseReporting = false;
    bool linkUrl = false;
    LinkPreviewMode linkPreviews = LinkPreviewMode::Never;

    QVector<GhosttyConfigFile> configFiles;
    bool quitAfterLastWindowClosed = false;
    std::optional<std::chrono::milliseconds> quitAfterLastWindowClosedDelay;
    bool initialWindow = false;
    SingleInstanceMode singleInstanceMode = SingleInstanceMode::Detect;

    bool operator==(const GhosttyConfigValues &) const = default;
};
