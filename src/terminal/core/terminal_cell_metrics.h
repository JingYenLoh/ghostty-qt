#pragma once

#include "terminal/core/terminal_font_resolver.h"
#include "terminal/model/terminal_typography.h"

#include <QFont>
#include <QStringView>
#include <QtTypes>

#include <memory>

struct TerminalCellMetrics {
    // Retains the weak-cached immutable program while this metrics generation
    // is live. Renderer snapshots compare and share this pointer instead of
    // copying four QFonts plus the interval table on every frame.
    std::shared_ptr<const TerminalFontProgram> fontProgram;
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

    [[nodiscard]] const QFont &font(TerminalFontRole role) const noexcept
    {
        Q_ASSERT(fontProgram != nullptr);
        return fontProgram->fonts[terminalEnumIndex(role)];
    }

    [[nodiscard]] const QFont &fontForText(TerminalFontRole role,
                                           QStringView text) const noexcept
    {
        Q_ASSERT(fontProgram != nullptr);
        return terminalFontForText(*fontProgram, role, text);
    }

    bool operator==(const TerminalCellMetrics &) const = default;
};

// Uses Qt's GUI font database and metrics. Call only on the GUI thread after
// constructing QGuiApplication. Invalid DPR values are treated as 1.
[[nodiscard]] TerminalCellMetrics
terminalCellMetrics(const TerminalTypography &typography,
                    qreal devicePixelRatio = 1.0);
