#pragma once

#include "terminal_types.h"

#include <QColor>
#include <QFont>
#include <QString>
#include <QVector>

#include <span>

// Ghostty permits background-only changes inside a shaped run. Every other
// terminal style attribute remains part of the compatibility key.
struct TerminalShapingStyle {
    QColor foreground;
    QColor underlineColor;
    TerminalColorSource foregroundSource = TerminalColorSource::Default;
    int foregroundPaletteIndex = -1;
    bool bold = false;
    bool italic = false;
    bool faint = false;
    bool textBlink = false;
    bool inverse = false;
    bool underlineUsesForeground = true;
    TerminalUnderlineStyle underlineStyle = TerminalUnderlineStyle::None;
    bool strikeThrough = false;
    bool overline = false;

    bool operator==(const TerminalShapingStyle &) const = default;
};

[[nodiscard]] TerminalShapingStyle
terminalShapingStyle(const TerminalCell &cell);

struct TerminalTextCell {
    QString text;
    QFont font;
    QColor color;
    TerminalShapingStyle style;
    quint32 baseCodepoint = 0;
    int column = 0;
    int columnSpan = 1;
    bool plainCodepoint = false;
    bool extendedGrapheme = false;
    bool selected = false;
    bool invisible = false;
    bool spacer = false;
    bool cursor = false;

    bool operator==(const TerminalTextCell &) const = default;
};

struct TerminalTextBoundary {
    // UTF-16 cursor position after this terminal cell.
    int textPosition = 0;
    // Grid-column offset after this terminal cell, relative to the run.
    int column = 0;
    bool placeholder = false;

    bool operator==(const TerminalTextBoundary &) const = default;
};

struct TerminalTextFallbackCell {
    QString text;
    int column = 0;
    int columnSpan = 1;

    bool operator==(const TerminalTextFallbackCell &) const = default;
};

struct TerminalTextRun {
    QString text;
    QFont font;
    QColor color;
    int column = 0;
    int columnSpan = 0;
    QVector<TerminalTextBoundary> boundaries;

    bool operator==(const TerminalTextRun &) const = default;
};

// Reconstructs exact cell text and grid geometry only for the native fallback
// path. Interior placeholders remain in TerminalTextRun::text for shaping but
// never produce a fallback cell.
[[nodiscard]] QVector<TerminalTextFallbackCell>
terminalTextFallbackCells(const TerminalTextRun &run);

[[nodiscard]] qsizetype terminalTextCellCount(const TerminalTextRun &run);

// Plans maximal compatible row runs. Empty interior cells become spaces so
// shaping observes their grid separation, while invisible cells and explicit
// spacer cells never enter the shaper. Trailing placeholders are discarded.
[[nodiscard]] QVector<TerminalTextRun>
planTerminalTextRuns(std::span<const TerminalTextCell> cells,
                     bool breakAtCursor);
