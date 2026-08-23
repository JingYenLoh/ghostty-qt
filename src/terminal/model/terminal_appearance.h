#pragma once

#include "terminal/model/terminal_backdrop_options.h"

#include <QColor>
#include <QMetaType>
#include <QVector>

#include <optional>

enum class TerminalColorKind {
    Unset,
    Color,
    CellForeground,
    CellBackground,
};

// A configured color may be a fixed RGB value or one of Ghostty's
// cell-relative aliases. Keeping the alias intact is important for search
// matches, selections, and cursor text, whose effective color can differ for
// every rendered cell.
struct TerminalColorValue {
    TerminalColorKind kind = TerminalColorKind::Unset;
    QColor color;

    static TerminalColorValue fromColor(const QColor &value)
    {
        return {.kind = TerminalColorKind::Color, .color = value};
    }

    bool operator==(const TerminalColorValue &) const = default;
};

enum class TerminalCursorStyle {
    Block,
    Bar,
    Underline,
    BlockHollow,
};

enum class TerminalBoldColorKind {
    Unset,
    Bright,
    Color,
};

struct TerminalBoldColor {
    TerminalBoldColorKind kind = TerminalBoldColorKind::Unset;
    QColor color;

    bool operator==(const TerminalBoldColor &) const = default;
};

// Value-only, Qt-thread-safe effective appearance. An empty palette is the
// LaunchOptions fallback used when no configuration snapshot exists, so
// libghostty retains its built-in palette. Every parsed Ghostty snapshot
// supplies all 256 effective entries after palette generation/harmonization.
struct TerminalAppearance {
    QColor foregroundColor = QColor(QStringLiteral("#d8dee9"));
    QColor backgroundColor = QColor(QStringLiteral("#1e222a"));
    QVector<QColor> palette;

    TerminalColorValue selectionForeground;
    TerminalColorValue selectionBackground;

    TerminalColorValue searchForeground =
        TerminalColorValue::fromColor(QColor(QStringLiteral("#000000")));
    TerminalColorValue searchBackground =
        TerminalColorValue::fromColor(QColor(QStringLiteral("#FFE082")));
    TerminalColorValue searchSelectedForeground =
        TerminalColorValue::fromColor(QColor(QStringLiteral("#000000")));
    TerminalColorValue searchSelectedBackground =
        TerminalColorValue::fromColor(QColor(QStringLiteral("#F2A57E")));

    TerminalColorValue cursorColor;
    TerminalCursorStyle cursorStyle = TerminalCursorStyle::Block;
    std::optional<bool> cursorBlink;
    double cursorOpacity = 1.0;
    TerminalColorValue cursorTextColor;

    TerminalBoldColor boldColor;
    double faintOpacity = 0.5;
    // Ghostty's WCAG threshold is evaluated by the renderer after faint
    // alpha and before the block-cursor text override.
    double minimumContrast = 1.0;

    bool operator==(const TerminalAppearance &) const = default;
};

Q_DECLARE_METATYPE(TerminalColorKind)
Q_DECLARE_METATYPE(TerminalColorValue)
Q_DECLARE_METATYPE(TerminalCursorStyle)
Q_DECLARE_METATYPE(TerminalBoldColorKind)
Q_DECLARE_METATYPE(TerminalBoldColor)
Q_DECLARE_METATYPE(TerminalAppearance)
