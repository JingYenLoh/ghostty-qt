#include "terminal_text_runs.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace {

[[nodiscard]] bool defensiveLigatureBoundary(const TerminalTextCell &left,
                                             const TerminalTextCell &right)
{
    if (!left.plainCodepoint || !right.plainCodepoint) {
        return false;
    }
    return (left.baseCodepoint == U'f'
            && (right.baseCodepoint == U'i'
                || right.baseCodepoint == U'l'))
        || (left.baseCodepoint == U's' && right.baseCodepoint == U't');
}

[[nodiscard]] bool cursorBoundary(const TerminalTextCell &left,
                                  const TerminalTextCell &right,
                                  bool enabled)
{
    return enabled
        && ((left.cursor && !left.extendedGrapheme)
            || (right.cursor && !right.extendedGrapheme));
}

[[nodiscard]] bool compatible(const TerminalTextCell &left,
                              const TerminalTextCell &right,
                              bool breakAtCursor)
{
    const int leftSpan = std::max(1, left.columnSpan);
    return left.column + leftSpan == right.column
        && left.font == right.font && left.color == right.color
        && left.style == right.style && left.selected == right.selected
        && !defensiveLigatureBoundary(left, right)
        && !cursorBoundary(left, right, breakAtCursor);
}

void trimAndAppend(std::optional<TerminalTextRun> &pending,
                   QVector<TerminalTextRun> &runs)
{
    if (!pending) {
        return;
    }

    TerminalTextRun run = std::move(*pending);
    pending.reset();
    while (!run.boundaries.isEmpty()
           && run.boundaries.constLast().placeholder) {
        run.boundaries.removeLast();
        const int textSize = run.boundaries.isEmpty()
            ? 0
            : run.boundaries.constLast().textPosition;
        run.text.truncate(textSize);
    }
    if (run.text.isEmpty() || run.fallbackCells.isEmpty()) {
        return;
    }
    run.columnSpan = run.boundaries.constLast().column;
    runs.append(std::move(run));
}

void appendCell(TerminalTextRun &run, const TerminalTextCell &cell)
{
    const bool placeholder = cell.text.isEmpty();
    run.text += placeholder ? QStringLiteral(" ") : cell.text;
    const int endColumn =
        cell.column + std::max(1, cell.columnSpan) - run.column;
    run.boundaries.append({
        .textPosition = static_cast<int>(run.text.size()),
        .column = endColumn,
        .placeholder = placeholder,
    });
    run.columnSpan = endColumn;
    if (!placeholder) {
        run.fallbackCells.append({
            .text = cell.text,
            .column = cell.column,
            .columnSpan = std::max(1, cell.columnSpan),
        });
    }
}

} // namespace

TerminalShapingStyle terminalShapingStyle(const TerminalCell &cell)
{
    return {
        .foreground = cell.foreground,
        .underlineColor = cell.underlineColor,
        .foregroundSource = cell.styleForegroundSource(),
        .foregroundPaletteIndex = cell.styleForegroundPaletteIndex(),
        .bold = cell.bold(),
        .italic = cell.italic(),
        .faint = cell.faint(),
        .textBlink = cell.textBlink(),
        .inverse = cell.inverse(),
        .underlineUsesForeground = cell.underlineUsesForeground(),
        .underlineStyle = cell.underlineStyle(),
        .strikeThrough = cell.strikeThrough(),
        .overline = cell.overline(),
    };
}

QVector<TerminalTextRun>
planTerminalTextRuns(std::span<const TerminalTextCell> cells,
                     bool breakAtCursor)
{
    QVector<TerminalTextRun> runs;
    // Most rows collapse into only a handful of runs. Keep the common case
    // allocation-free without reserving one run object per terminal column.
    runs.reserve(
        static_cast<qsizetype>(std::min<std::size_t>(cells.size(), 16)));

    std::optional<TerminalTextRun> pending;
    const TerminalTextCell *previous = nullptr;
    for (const TerminalTextCell &cell : cells) {
        // Wide-cell spacer records belong to the preceding head and neither
        // contribute text nor create an artificial shaping boundary.
        if (cell.spacer) {
            continue;
        }
        if (cell.invisible) {
            trimAndAppend(pending, runs);
            previous = nullptr;
            continue;
        }

        if (pending && previous
            && !compatible(*previous, cell, breakAtCursor)) {
            trimAndAppend(pending, runs);
        }

        if (!pending && !cell.text.isEmpty()) {
            pending.emplace();
            pending->font = cell.font;
            pending->color = cell.color;
            pending->column = cell.column;
        }
        if (pending) {
            appendCell(*pending, cell);
        }
        previous = &cell;
    }
    trimAndAppend(pending, runs);
    return runs;
}
