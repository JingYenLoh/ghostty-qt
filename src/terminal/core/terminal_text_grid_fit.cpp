#include "terminal/core/terminal_text_grid_fit.h"

#include <QGlyphRun>
#include <QTextLayout>

#include <algorithm>
#include <cmath>

namespace {

[[nodiscard]] qreal normalizedDevicePixelRatio(qreal value) noexcept
{
    return std::isfinite(value) && value > 0.0 ? value : 1.0;
}

[[nodiscard]] bool boundaryFitsGrid(const QTextLine &line,
                                    const TerminalTextBoundary &boundary,
                                    qreal cellWidth, qreal devicePixelRatio)
{
    const qint64 actual =
        qRound64(line.cursorToX(boundary.textPosition) * devicePixelRatio);
    const qint64 expected = qRound64(static_cast<qreal>(boundary.column)
                                     * cellWidth * devicePixelRatio);
    return actual == expected;
}

[[nodiscard]] bool hasValidBoundaryMap(const QTextLine &line,
                                       const TerminalTextRun &run)
{
    if (!line.isValid() || line.textStart() != 0 || run.text.isEmpty()
        || run.boundaries.isEmpty() || run.columnSpan <= 0) {
        return false;
    }

    int previousTextPosition = 0;
    int previousColumn = 0;
    for (const TerminalTextBoundary &boundary : run.boundaries) {
        if (boundary.textPosition <= previousTextPosition
            || boundary.textPosition > run.text.size()
            || boundary.column <= previousColumn
            || boundary.column > run.columnSpan) {
            return false;
        }
        previousTextPosition = boundary.textPosition;
        previousColumn = boundary.column;
    }
    return previousTextPosition == run.text.size()
        && previousColumn == run.columnSpan
        && line.textLength() == run.text.size();
}

} // namespace

TerminalTextGridFit terminalTextGridFit(const QTextLine &line,
                                        const TerminalTextRun &run,
                                        qreal cellWidth, qreal devicePixelRatio)
{
    if (!std::isfinite(cellWidth) || cellWidth <= 0.0
        || !hasValidBoundaryMap(line, run)) {
        return TerminalTextGridFit::Rejected;
    }

    const qreal dpr = normalizedDevicePixelRatio(devicePixelRatio);
    if (std::ranges::all_of(
            run.boundaries, [&](const TerminalTextBoundary &boundary) {
                return boundaryFitsGrid(line, boundary, cellWidth, dpr);
            })) {
        return TerminalTextGridFit::Exact;
    }

    QVector<int> clusterStarts;
    const QList<QGlyphRun> glyphRuns = line.glyphRuns(
        -1, -1,
        QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveStringIndexes);
    for (const QGlyphRun &glyphRun : glyphRuns) {
        const QList<quint32> glyphIndexes = glyphRun.glyphIndexes();
        const QList<qsizetype> stringIndexes = glyphRun.stringIndexes();
        // A glyph run without the requested mapping makes it impossible to
        // prove that a mismatched terminal boundary is inside a cluster.
        if (glyphIndexes.size() != stringIndexes.size()) {
            return TerminalTextGridFit::Rejected;
        }
        for (const qsizetype stringIndex : stringIndexes) {
            if (stringIndex < 0 || stringIndex >= run.text.size()) {
                return TerminalTextGridFit::Rejected;
            }
            clusterStarts.append(static_cast<int>(stringIndex));
        }
    }
    if (clusterStarts.isEmpty()) {
        return TerminalTextGridFit::Rejected;
    }

    std::ranges::sort(clusterStarts);
    const auto uniqueEnd = std::ranges::unique(clusterStarts).begin();
    clusterStarts.erase(uniqueEnd, clusterStarts.end());
    // A missing initial cluster can hide an unshaped/control prefix. Preserve
    // the cell fallback instead of inferring its geometry.
    if (clusterStarts.front() != 0) {
        return TerminalTextGridFit::Rejected;
    }

    for (const TerminalTextBoundary &boundary : run.boundaries) {
        if (boundaryFitsGrid(line, boundary, cellWidth, dpr)) {
            continue;
        }
        const int textPosition = boundary.textPosition;
        const auto next = std::ranges::lower_bound(clusterStarts, textPosition);
        // Exposed cluster boundaries must always fit the cell grid. Only a
        // cursor position strictly inside one multi-code-unit cluster may
        // legitimately differ from its terminal-cell boundary.
        if (next == clusterStarts.begin()
            || (next != clusterStarts.end() && *next == textPosition)) {
            return TerminalTextGridFit::Rejected;
        }
        const int clusterEnd = next != clusterStarts.end()
            ? *next
            : static_cast<int>(run.text.size());
        if (textPosition >= clusterEnd) {
            return TerminalTextGridFit::Rejected;
        }
    }
    return TerminalTextGridFit::ShapedClusters;
}
