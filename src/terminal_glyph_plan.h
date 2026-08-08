#pragma once

#include "terminal_text_grid_fit.h"

#include <QPointF>
#include <QRawFont>
#include <QVector>
#include <QtTypes>

#include <optional>

class QTextLine;

// One glyph positioned by QTextLayout. The baseline position is expressed in
// the caller's coordinate system; sourceIndex is the corresponding UTF-16
// offset in TerminalTextRun::text.
struct TerminalGlyphInstance {
    QRawFont font;
    quint32 glyphIndex = 0;
    QPointF baselinePosition;
    qsizetype sourceIndex = 0;

    bool operator==(const TerminalGlyphInstance &) const = default;
};

using TerminalGlyphPlan = QVector<TerminalGlyphInstance>;

// Extracts the exact glyphs already shaped by Qt for the narrow, common
// terminal fast path. This never shapes text itself. Anything that cannot be
// proven to be one printable ASCII glyph per fixed-pitch grid cell fails as a
// complete unit so the renderer can retain its general QTextLayout path.
[[nodiscard]] std::optional<TerminalGlyphPlan>
terminalGlyphPlan(const TerminalTextRun &run, const QTextLine &line,
                  TerminalTextGridFit fit, const QPointF &origin);
