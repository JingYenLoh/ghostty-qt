#include "terminal_text_runs.h"

#include <algorithm>
#include <optional>
#include <utility>

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

QVector<TerminalTextFallbackCell>
terminalTextFallbackCells(const TerminalTextRun &run)
{
    QVector<TerminalTextFallbackCell> result;
    result.reserve(run.boundaries.size());

    int previousTextPosition = 0;
    int previousColumn = 0;
    for (const TerminalTextBoundary &boundary : run.boundaries) {
        if (boundary.textPosition <= previousTextPosition
            || boundary.textPosition > run.text.size()
            || boundary.column <= previousColumn
            || boundary.column > run.columnSpan) {
            return {};
        }

        if (!boundary.placeholder) {
            result.append({
                .text = run.text.sliced(previousTextPosition,
                                        boundary.textPosition
                                            - previousTextPosition),
                .column = run.column + previousColumn,
                .columnSpan = boundary.column - previousColumn,
            });
        }
        previousTextPosition = boundary.textPosition;
        previousColumn = boundary.column;
    }
    if (previousTextPosition != run.text.size()
        || previousColumn != run.columnSpan) {
        return {};
    }
    return result;
}

qsizetype terminalTextCellCount(const TerminalTextRun &run)
{
    return static_cast<qsizetype>(std::ranges::count_if(
        run.boundaries, [](const TerminalTextBoundary &boundary) {
            return !boundary.placeholder;
        }));
}

TerminalTextRunBuilder::TerminalTextRunBuilder(qsizetype expectedCellCount,
                                               bool breakAtCursor)
    : expectedCellCount_(std::max<qsizetype>(0, expectedCellCount))
    , breakAtCursor_(breakAtCursor)
{
    // Most rows collapse into only a handful of runs. Keep fragmented rows
    // bounded without reserving one run object per terminal column.
    runs_.reserve(std::min<qsizetype>(expectedCellCount_, 16));
}

Q_ALWAYS_INLINE bool
TerminalTextRunBuilder::compatibleWithPending(TerminalTextCellView cell) const
{
    Q_ASSERT(pending_.has_value());
    Q_ASSERT(previous_.has_value());
    const PreviousCell &left = *previous_;
    const int leftSpan = std::max(1, left.columnSpan);
    const bool defensiveLigatureBoundary = left.plainCodepoint
        && cell.plainCodepoint
        && ((left.baseCodepoint == U'f'
             && (cell.baseCodepoint == U'i' || cell.baseCodepoint == U'l'))
            || (left.baseCodepoint == U's' && cell.baseCodepoint == U't'));
    const bool cursorBoundary = breakAtCursor_
        && ((left.cursor && !left.extendedGrapheme)
            || (cell.cursor && !cell.extendedGrapheme));
    return left.column + leftSpan == cell.column && pending_->font == cell.font
        && pending_->color == cell.color && pendingStyle_ == cell.style
        && pendingSelected_ == cell.selected && !defensiveLigatureBoundary
        && !cursorBoundary;
}

void TerminalTextRunBuilder::startPending(TerminalTextCellView cell)
{
    Q_ASSERT(!pending_.has_value());
    pending_.emplace();
    pending_->font = cell.font;
    pending_->color = cell.color;
    pending_->column = cell.column;
    pendingStyle_ = cell.style;
    pendingSelected_ = cell.selected;

    // The common terminal row is one run. Give its text and boundary arrays
    // their final grid-sized capacity once; later fragmented runs retain the
    // normal geometric growth policy instead of repeatedly over-reserving.
    if (!reservedFullRow_ && expectedCellCount_ > 0) {
        pending_->text.reserve(expectedCellCount_);
        pending_->boundaries.reserve(expectedCellCount_);
        reservedFullRow_ = true;
    }
}

Q_ALWAYS_INLINE void
TerminalTextRunBuilder::appendToPending(TerminalTextCellView cell)
{
    Q_ASSERT(pending_.has_value());
    const bool placeholder = cell.text.isEmpty();
    if (placeholder) {
        pending_->text += QLatin1Char(' ');
    } else {
        pending_->text += cell.text;
    }
    const int endColumn =
        cell.column + std::max(1, cell.columnSpan) - pending_->column;
    pending_->boundaries.append({
        .textPosition = static_cast<int>(pending_->text.size()),
        .column = endColumn,
        .placeholder = placeholder,
    });
    pending_->columnSpan = endColumn;
}

void TerminalTextRunBuilder::finishPending()
{
    if (!pending_) {
        return;
    }

    TerminalTextRun run = std::move(*pending_);
    pending_.reset();
    while (!run.boundaries.isEmpty()
           && run.boundaries.constLast().placeholder) {
        run.boundaries.removeLast();
        const int textSize = run.boundaries.isEmpty()
            ? 0
            : run.boundaries.constLast().textPosition;
        run.text.truncate(textSize);
    }
    if (run.text.isEmpty() || run.boundaries.isEmpty()) {
        return;
    }
    run.columnSpan = run.boundaries.constLast().column;
    runs_.append(std::move(run));
}

void TerminalTextRunBuilder::append(TerminalTextCellView cell)
{
    // Wide-cell spacer records belong to the preceding head and neither
    // contribute text nor create an artificial shaping boundary.
    if (cell.spacer) {
        return;
    }
    if (cell.invisible) {
        finishPending();
        previous_.reset();
        return;
    }

    if (pending_ && previous_ && !compatibleWithPending(cell)) {
        finishPending();
    }

    if (!pending_ && !cell.text.isEmpty()) {
        startPending(cell);
    }
    if (pending_) {
        appendToPending(cell);
    }
    previous_ = PreviousCell{
        .baseCodepoint = cell.baseCodepoint,
        .column = cell.column,
        .columnSpan = cell.columnSpan,
        .plainCodepoint = cell.plainCodepoint,
        .extendedGrapheme = cell.extendedGrapheme,
        .cursor = cell.cursor,
    };
}

QVector<TerminalTextRun> TerminalTextRunBuilder::takeRuns() &&
{
    finishPending();
    return std::move(runs_);
}

QVector<TerminalTextRun>
planTerminalTextRuns(std::span<const TerminalTextCell> cells,
                     bool breakAtCursor)
{
    TerminalTextRunBuilder builder(static_cast<qsizetype>(cells.size()),
                                   breakAtCursor);
    for (const TerminalTextCell &cell : cells) {
        builder.append({
            .text = cell.text,
            .font = cell.font,
            .color = cell.color,
            .style = cell.style,
            .baseCodepoint = cell.baseCodepoint,
            .column = cell.column,
            .columnSpan = cell.columnSpan,
            .plainCodepoint = cell.plainCodepoint,
            .extendedGrapheme = cell.extendedGrapheme,
            .selected = cell.selected,
            .invisible = cell.invisible,
            .spacer = cell.spacer,
            .cursor = cell.cursor,
        });
    }
    return std::move(builder).takeRuns();
}
