#pragma once

#include "terminal/core/terminal_text_runs.h"

#include <QtTypes>

class QTextLine;

enum class TerminalTextGridFit {
    Rejected,
    Exact,
    ShapedClusters,
};

// Validates a shaped terminal row run against its physical cell grid. Exact
// cursor positions are the common fast path. A mismatch is accepted only when
// Qt's glyph metadata proves that the position is internal to one shaping
// cluster and every exposed cluster boundary still lands on the grid.
[[nodiscard]] TerminalTextGridFit
terminalTextGridFit(const QTextLine &line, const TerminalTextRun &run,
                    qreal cellWidth, qreal devicePixelRatio);
