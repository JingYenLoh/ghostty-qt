#pragma once

#include "terminal_text_grid_fit.h"

#include <QPointF>
#include <QRawFont>
#include <QVector>
#include <QtTypes>

#include <optional>

class QTextLine;

// One glyph positioned by QTextLayout. The baseline position is expressed in
// the caller's coordinate system. Source coverage is validated while building
// the plan but is not retained because renderers do not consume it.
struct TerminalGlyphInstance {
    QRawFont font;
    QPointF baselinePosition;
    quint32 glyphIndex = 0;

    bool operator==(const TerminalGlyphInstance &) const = default;
};

#if defined(Q_PROCESSOR_X86_64)
static_assert(sizeof(TerminalGlyphInstance) == 32);
#endif

using TerminalGlyphPlan = QVector<TerminalGlyphInstance>;

// Extracts the exact glyphs already shaped by Qt for the narrow, common
// terminal fast path. This never shapes text itself. Anything that cannot be
// proven to be one printable ASCII glyph per fixed-pitch grid cell fails as a
// complete unit so the renderer can retain its general QTextLayout path.
[[nodiscard]] std::optional<TerminalGlyphPlan>
terminalGlyphPlan(const TerminalTextRun &run, const QTextLine &line,
                  TerminalTextGridFit fit, const QPointF &origin);
