#pragma once

#include "terminal_types.h"

#include <QPoint>
#include <QVector>

// Worker-side sparse view of the OSC 8 presence bits in the rendered
// viewport. Rows are updated from the same value-only deltas sent to the GUI;
// no terminal handles or full TerminalCell copies are retained here.
class TerminalOsc8Index final {
public:
    [[nodiscard]] bool apply(const TerminalUpdate &update)
    {
        if (!validShape(update)
            || (!update.fullFrame
                && (!hasFrame() || columns_ != update.columns
                    || rows_ != update.rows))) {
            return false;
        }

        if (update.fullFrame) {
            columns_ = update.columns;
            rows_ = update.rows;
            linkedColumnsByRow_.resize(rows_);
        }

        bool candidatesChanged = update.fullFrame;
        for (const TerminalRowUpdate &row : update.dirtyRows) {
            QVector<int> &linkedColumns = linkedColumnsByRow_[row.row];
            if (matches(linkedColumns, row)) {
                continue;
            }
            assign(linkedColumns, row);
            candidatesChanged = true;
        }
        candidatesDirty_ = candidatesDirty_ || candidatesChanged;
        contentRevision_ = update.contentRevision;
        return true;
    }

    void clear()
    {
        columns_ = 0;
        rows_ = 0;
        contentRevision_ = 0;
        linkedColumnsByRow_.clear();
        candidates_.clear();
        candidatesDirty_ = false;
    }

    [[nodiscard]] bool hasFrame() const noexcept
    {
        return columns_ > 0 && rows_ > 0 && linkedColumnsByRow_.size() == rows_;
    }

    [[nodiscard]] bool containsCoordinate(int column, int row) const noexcept
    {
        return hasFrame() && column >= 0 && column < columns_ && row >= 0
            && row < rows_;
    }

    [[nodiscard]] int columns() const noexcept { return columns_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] quint64 contentRevision() const noexcept
    {
        return contentRevision_;
    }

    [[nodiscard]] const QVector<QPoint> &candidates() const
    {
        if (!candidatesDirty_) {
            return candidates_;
        }

        qsizetype candidateCount = 0;
        for (const QVector<int> &linkedColumns : linkedColumnsByRow_) {
            candidateCount += linkedColumns.size();
        }
        candidates_.clear();
        candidates_.reserve(candidateCount);
        for (int row = 0; row < rows_; ++row) {
            for (int column : linkedColumnsByRow_.at(row)) {
                candidates_.append(QPoint(column, row));
            }
        }
        candidatesDirty_ = false;
        return candidates_;
    }

private:
    [[nodiscard]] static bool matches(const QVector<int> &linkedColumns,
                                      const TerminalRowUpdate &row)
    {
        qsizetype linkedIndex = 0;
        for (int column = 0; column < row.cells.size(); ++column) {
            if (!row.cells.at(column).hasHyperlink()) {
                continue;
            }
            if (linkedIndex >= linkedColumns.size()
                || linkedColumns.at(linkedIndex) != column) {
                return false;
            }
            ++linkedIndex;
        }
        return linkedIndex == linkedColumns.size();
    }

    static void assign(QVector<int> &linkedColumns,
                       const TerminalRowUpdate &row)
    {
        linkedColumns.clear();
        for (int column = 0; column < row.cells.size(); ++column) {
            if (row.cells.at(column).hasHyperlink()) {
                linkedColumns.append(column);
            }
        }
    }

    [[nodiscard]] static bool validShape(const TerminalUpdate &update)
    {
        if (!validTerminalUpdateShape(update)) {
            return false;
        }
        const qsizetype columns = update.columns;
        const qsizetype rows = update.rows;
        return columns <= QVector<QPoint>::maxSize() / rows;
    }

    int columns_ = 0;
    int rows_ = 0;
    quint64 contentRevision_ = 0;
    QVector<QVector<int>> linkedColumnsByRow_;
    mutable QVector<QPoint> candidates_;
    mutable bool candidatesDirty_ = false;
};
