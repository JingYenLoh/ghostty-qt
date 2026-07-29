#pragma once

#include "terminal_font_resolver.h"
#include "terminal_typography.h"

#include <QFont>
#include <QStringView>
#include <QtTypes>

#include <array>

struct TerminalCellMetrics {
    std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)> fonts;
    QVector<TerminalMappedFont> mappedFonts;
    bool shapingBreakCursor = true;

    // All geometry is expressed in logical scene units. Each value is derived
    // from an integral number of physical pixels at the requested DPR.
    qreal cellWidth = 1.0;
    qreal cellHeight = 1.0;
    qreal baseline = 1.0;
    qreal underlinePosition = 1.0;
    qreal underlineThickness = 1.0;
    qreal strikethroughPosition = 1.0;
    qreal strikethroughThickness = 1.0;
    qreal overlinePosition = 0.0;
    qreal overlineThickness = 1.0;
    qreal cursorThickness = 1.0;
    qreal cursorHeight = 1.0;
    qreal cursorTop = 0.0;
    qreal cursorBarLeft = -1.0;
    qreal underlineMaximumPosition = 1.0;
    qreal overlineMinimumPosition = 0.0;

    [[nodiscard]] QFont &font(TerminalFontRole role) noexcept
    {
        return fonts[terminalEnumIndex(role)];
    }

    [[nodiscard]] const QFont &font(TerminalFontRole role) const noexcept
    {
        return fonts[terminalEnumIndex(role)];
    }

    [[nodiscard]] const QFont &fontForText(TerminalFontRole role,
                                           QStringView text) const noexcept
    {
        return terminalFontForText(fonts, mappedFonts, role, text);
    }

    bool operator==(const TerminalCellMetrics &) const = default;
};

// Uses Qt's GUI font database and metrics. Call only on the GUI thread after
// constructing QGuiApplication. Invalid DPR values are treated as 1.
[[nodiscard]] TerminalCellMetrics
terminalCellMetrics(const TerminalTypography &typography,
                    qreal devicePixelRatio = 1.0);
