#include "terminal_glyph_plan.h"

#include <QGlyphRun>
#include <QTextLayout>

#include <cmath>

namespace {

[[nodiscard]] bool finitePoint(const QPointF &point) noexcept
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

[[nodiscard]] bool hasPlainAsciiCellMap(const TerminalTextRun &run)
{
    if (!run.font.fixedPitch() || run.text.isEmpty() || run.boundaries.isEmpty()
        || run.columnSpan <= 0) {
        return false;
    }

    int previousTextPosition = 0;
    int previousColumn = 0;
    for (const TerminalTextBoundary &boundary : run.boundaries) {
        if (boundary.textPosition != previousTextPosition + 1
            || boundary.column != previousColumn + 1
            || boundary.textPosition > run.text.size()
            || boundary.column > run.columnSpan) {
            return false;
        }
        const ushort codeUnit = run.text.at(previousTextPosition).unicode();
        if (codeUnit < 0x20 || codeUnit > 0x7e) {
            return false;
        }
        previousTextPosition = boundary.textPosition;
        previousColumn = boundary.column;
    }
    return previousTextPosition == run.text.size()
        && previousColumn == run.columnSpan;
}

} // namespace

std::optional<TerminalGlyphPlan> terminalGlyphPlan(const TerminalTextRun &run,
                                                   const QTextLine &line,
                                                   TerminalTextGridFit fit,
                                                   const QPointF &origin)
{
    if (fit != TerminalTextGridFit::Exact || !line.isValid()
        || line.textStart() != 0 || line.textLength() != run.text.size()
        || !finitePoint(origin) || !hasPlainAsciiCellMap(run)) {
        return std::nullopt;
    }

    constexpr QTextLayout::GlyphRunRetrievalFlags retrievalFlags =
        QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveGlyphPositions
        | QTextLayout::RetrieveStringIndexes;
    const QList<QGlyphRun> glyphRuns = line.glyphRuns(-1, -1, retrievalFlags);

    TerminalGlyphPlan result;
    result.reserve(run.text.size());
    QVector<bool> mappedSources(run.text.size(), false);
    for (const QGlyphRun &glyphRun : glyphRuns) {
        if (glyphRun.flags() != QGlyphRun::GlyphRunFlags{}) {
            return std::nullopt;
        }

        const QRawFont font = glyphRun.rawFont();
        const QList<quint32> glyphIndexes = glyphRun.glyphIndexes();
        const QList<QPointF> positions = glyphRun.positions();
        const QList<qsizetype> stringIndexes = glyphRun.stringIndexes();
        if (!font.isValid() || glyphIndexes.isEmpty()
            || glyphIndexes.size() != positions.size()
            || glyphIndexes.size() != stringIndexes.size()) {
            return std::nullopt;
        }

        for (qsizetype index = 0; index < glyphIndexes.size(); ++index) {
            const qsizetype sourceIndex = stringIndexes.at(index);
            const QPointF baselinePosition = origin + positions.at(index);
            if (sourceIndex < 0 || sourceIndex >= run.text.size()
                || mappedSources.at(sourceIndex)
                || !finitePoint(positions.at(index))
                || !finitePoint(baselinePosition)) {
                return std::nullopt;
            }
            mappedSources[sourceIndex] = true;
            result.append({
                .font = font,
                .glyphIndex = glyphIndexes.at(index),
                .baselinePosition = baselinePosition,
                .sourceIndex = sourceIndex,
            });
        }
    }

    if (result.size() != run.text.size()) {
        return std::nullopt;
    }
    for (const bool mapped : mappedSources) {
        if (!mapped) return std::nullopt;
    }
    return result;
}
