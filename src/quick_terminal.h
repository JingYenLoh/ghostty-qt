#pragma once

#include <QMargins>
#include <QRect>
#include <QSize>
#include <QtGlobal>

#include <bit>
#include <optional>
#include <variant>

enum class QuickTerminalPosition {
    Top,
    Bottom,
    Left,
    Right,
    Center,
};

enum class QuickTerminalScreen {
    Main,
    Mouse,
    MacosMenuBar,
};

enum class QuickTerminalKeyboardInteractivity {
    None,
    OnDemand,
    Exclusive,
};

struct QuickTerminalPercentage {
    float value = 0.0F;

    friend bool operator==(QuickTerminalPercentage left,
                           QuickTerminalPercentage right) noexcept
    {
        return std::bit_cast<quint32>(left.value)
            == std::bit_cast<quint32>(right.value);
    }
};

struct QuickTerminalPixels {
    quint32 value = 0;

    bool operator==(const QuickTerminalPixels &) const = default;
};

using QuickTerminalExtent =
    std::variant<QuickTerminalPercentage, QuickTerminalPixels>;

struct QuickTerminalSize {
    std::optional<QuickTerminalExtent> primary;
    std::optional<QuickTerminalExtent> secondary;

    bool operator==(const QuickTerminalSize &) const = default;
};

struct QuickTerminalOptions {
    QuickTerminalPosition position = QuickTerminalPosition::Top;
    QuickTerminalSize size;
    QuickTerminalScreen screen = QuickTerminalScreen::Main;
    bool autohide = false;
    QuickTerminalKeyboardInteractivity keyboardInteractivity =
        QuickTerminalKeyboardInteractivity::OnDemand;

    bool operator==(const QuickTerminalOptions &) const = default;
};

inline constexpr int kQuickTerminalUnanchoredMargin = 20;

// Mirrors the pinned Ghostty layer-shell intent independently from fallback
// window geometry: the configured edge is anchored with no margin, while each
// unanchored edge carries a 20-logical-pixel margin.
struct QuickTerminalPlacementIntent {
    Qt::Edges anchors;
    QMargins margins;

    bool operator==(const QuickTerminalPlacementIntent &) const = default;
};

[[nodiscard]] int quickTerminalExtentPixels(const QuickTerminalExtent &extent,
                                            int parentDimension) noexcept;

// Calculates the requested logical size exactly like Ghostty's pinned Zig
// QuickTerminalSize.calculate. Percentage arithmetic remains f32 and truncates
// toward zero. Values outside QSize's nonnegative int domain are saturated.
[[nodiscard]] QSize quickTerminalSize(const QuickTerminalSize &size,
                                      QuickTerminalPosition position,
                                      QSize fullOutputSize) noexcept;

[[nodiscard]] QuickTerminalPlacementIntent quickTerminalPlacementIntent(
    QuickTerminalPosition position,
    int unanchoredMargin = kQuickTerminalUnanchoredMargin) noexcept;

// Places a requested size against an output without resizing it. Margins are
// represented separately by quickTerminalPlacementIntent because ordinary Qt
// window geometry cannot express layer-shell's anchor/margin contract.
[[nodiscard]] QRect quickTerminalGeometry(QuickTerminalPosition position,
                                          QRect outputGeometry,
                                          QSize requestedSize) noexcept;

[[nodiscard]] QRect quickTerminalGeometry(const QuickTerminalOptions &options,
                                          QRect outputGeometry) noexcept;
