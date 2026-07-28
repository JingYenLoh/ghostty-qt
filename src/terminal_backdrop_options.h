#pragma once

#include "ghostty_config_path.h"

#include <QtGlobal>

#include <optional>

struct TerminalPaddingAxis {
    quint32 leadingPoints = 0;
    quint32 trailingPoints = 0;

    bool operator==(const TerminalPaddingAxis &) const = default;
};

enum class TerminalPaddingBalance : quint8 {
    Disabled,
    Balanced,
    Equal,
};

enum class TerminalPaddingColor : quint8 {
    Background,
    Extend,
    ExtendAlways,
};

// The dimensions are captured when a pane is constructed because Ghostty
// documents window-padding-x/y as new-terminal-only settings. Balance and
// color remain in the same value so layout and painting share one policy.
struct TerminalPaddingOptions {
    TerminalPaddingAxis horizontal;
    TerminalPaddingAxis vertical;
    TerminalPaddingBalance balance = TerminalPaddingBalance::Disabled;
    TerminalPaddingColor color = TerminalPaddingColor::Background;

    bool operator==(const TerminalPaddingOptions &) const = default;

    [[nodiscard]] static constexpr TerminalPaddingOptions none() noexcept
    {
        return {};
    }

    [[nodiscard]] static constexpr TerminalPaddingOptions
    ghosttyDefault() noexcept
    {
        return {
            .horizontal = {.leadingPoints = 2, .trailingPoints = 2},
            .vertical = {.leadingPoints = 2, .trailingPoints = 2},
        };
    }
};

enum class TerminalBackgroundImagePosition : quint8 {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

enum class TerminalBackgroundImageFit : quint8 {
    Contain,
    Cover,
    Stretch,
    None,
};

struct TerminalBackgroundImageOptions {
    std::optional<GhosttyConfigPath> path;
    // Ghostty deliberately accepts values above one: the image may be more
    // opaque than the surrounding terminal background.
    double opacity = 1.0;
    TerminalBackgroundImagePosition position =
        TerminalBackgroundImagePosition::Center;
    TerminalBackgroundImageFit fit = TerminalBackgroundImageFit::Contain;
    bool repeat = false;

    bool operator==(const TerminalBackgroundImageOptions &) const = default;
};

// GUI-owned compositing policy. It never enters SessionWorker: existing panes
// repaint in place when these values are live-reloaded.
struct TerminalBackgroundOptions {
    double opacity = 1.0;
    bool opacityCells = false;
    TerminalBackgroundImageOptions image;

    bool operator==(const TerminalBackgroundOptions &) const = default;
};
