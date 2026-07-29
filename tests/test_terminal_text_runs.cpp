#include "terminal_text_runs.h"

#include <QTest>

#include <initializer_list>
#include <span>

namespace {

TerminalTextCell cell(QString text, int column)
{
    QFont font(QStringLiteral("monospace"));
    font.setFixedPitch(true);
    const quint32 codepoint =
        text.size() == 1 ? text.front().unicode() : 0;
    return {
        .text = std::move(text),
        .font = std::move(font),
        .color = QColor(Qt::white),
        .style = {},
        .baseCodepoint = codepoint,
        .column = column,
        .columnSpan = 1,
        .plainCodepoint = codepoint != 0,
    };
}

QVector<TerminalTextRun>
plan(std::initializer_list<TerminalTextCell> cells,
     bool breakAtCursor = true)
{
    return planTerminalTextRuns(
        std::span<const TerminalTextCell>(cells.begin(), cells.size()),
        breakAtCursor);
}

} // namespace

class TerminalTextRunsTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void combinesCompatibleCells();
    void keepsInteriorButNotTrailingEmptyCells();
    void ignoresLeadingEmptyAndEmptyRows();
    void breaksOnSelectionStyleFontAndInvisibility();
    void skipsWideSpacersWithoutBreaking();
    void blocksDefensiveLigaturesOnlyForPlainCodepoints();
    void isolatesCursorCellsWithoutExtraGraphemesWhenEnabled();
    void excludesBackgroundFromTheStyleKey();
};

void TerminalTextRunsTest::combinesCompatibleCells()
{
    const QVector<TerminalTextRun> runs =
        plan({cell(QStringLiteral("a"), 0), cell(QStringLiteral("b"), 1),
              cell(QStringLiteral("c"), 2)});

    QCOMPARE(runs.size(), 1);
    const TerminalTextRun &run = runs.front();
    QCOMPARE(run.text, QStringLiteral("abc"));
    QCOMPARE(run.column, 0);
    QCOMPARE(run.columnSpan, 3);
    QCOMPARE(run.boundaries,
             QVector<TerminalTextBoundary>({
                 {.textPosition = 1, .column = 1},
                 {.textPosition = 2, .column = 2},
                 {.textPosition = 3, .column = 3},
             }));
    QCOMPARE(run.fallbackCells.size(), 3);
}

void TerminalTextRunsTest::keepsInteriorButNotTrailingEmptyCells()
{
    const QVector<TerminalTextRun> runs =
        plan({cell(QStringLiteral("a"), 0), cell({}, 1),
              cell(QStringLiteral("b"), 2), cell({}, 3), cell({}, 4)});

    QCOMPARE(runs.size(), 1);
    const TerminalTextRun &run = runs.front();
    QCOMPARE(run.text, QStringLiteral("a b"));
    QCOMPARE(run.columnSpan, 3);
    QCOMPARE(run.boundaries.size(), 3);
    QCOMPARE(run.boundaries.at(1),
             TerminalTextBoundary(
                 {.textPosition = 2, .column = 2, .placeholder = true}));
    QCOMPARE(run.fallbackCells.size(), 2);
}

void TerminalTextRunsTest::ignoresLeadingEmptyAndEmptyRows()
{
    QCOMPARE(plan({cell({}, 0), cell({}, 1)}).size(), 0);

    const QVector<TerminalTextRun> runs =
        plan({cell({}, 0), cell({}, 1), cell(QStringLiteral("x"), 2)});
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.front().text, QStringLiteral("x"));
    QCOMPARE(runs.front().column, 2);
    QCOMPARE(runs.front().columnSpan, 1);
}

void TerminalTextRunsTest::breaksOnSelectionStyleFontAndInvisibility()
{
    TerminalTextCell selected = cell(QStringLiteral("b"), 1);
    selected.selected = true;
    TerminalTextCell styled = cell(QStringLiteral("c"), 2);
    styled.style.bold = true;
    TerminalTextCell otherFont = cell(QStringLiteral("d"), 3);
    otherFont.font.setPointSizeF(otherFont.font.pointSizeF() + 1.0);
    TerminalTextCell invisible = cell(QStringLiteral("e"), 4);
    invisible.invisible = true;

    const QVector<TerminalTextRun> runs =
        plan({cell(QStringLiteral("a"), 0), selected, styled, otherFont,
              invisible, cell(QStringLiteral("f"), 5)});

    QCOMPARE(runs.size(), 5);
    QCOMPARE(runs.at(0).text, QStringLiteral("a"));
    QCOMPARE(runs.at(1).text, QStringLiteral("b"));
    QCOMPARE(runs.at(2).text, QStringLiteral("c"));
    QCOMPARE(runs.at(3).text, QStringLiteral("d"));
    QCOMPARE(runs.at(4).text, QStringLiteral("f"));
}

void TerminalTextRunsTest::skipsWideSpacersWithoutBreaking()
{
    TerminalTextCell wide = cell(QStringLiteral("界"), 0);
    wide.columnSpan = 2;
    TerminalTextCell spacer = cell({}, 1);
    spacer.spacer = true;

    const QVector<TerminalTextRun> runs =
        plan({wide, spacer, cell(QStringLiteral("x"), 2)});

    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.front().text, QStringLiteral("界x"));
    QCOMPARE(runs.front().columnSpan, 3);
    QCOMPARE(runs.front().boundaries.at(0).column, 2);
    QCOMPARE(runs.front().boundaries.at(1).column, 3);
}

void TerminalTextRunsTest::blocksDefensiveLigaturesOnlyForPlainCodepoints()
{
    const QVector<TerminalTextRun> runs =
        plan({cell(QStringLiteral("a"), 0), cell(QStringLiteral("f"), 1),
              cell(QStringLiteral("i"), 2), cell(QStringLiteral("b"), 3),
              cell(QStringLiteral("s"), 4), cell(QStringLiteral("t"), 5)});

    QCOMPARE(runs.size(), 3);
    QCOMPARE(runs.at(0).text, QStringLiteral("af"));
    QCOMPARE(runs.at(1).text, QStringLiteral("ibs"));
    QCOMPARE(runs.at(2).text, QStringLiteral("t"));

    TerminalTextCell grapheme = cell(QStringLiteral("f"), 0);
    grapheme.plainCodepoint = false;
    const QVector<TerminalTextRun> graphemeRuns =
        plan({grapheme, cell(QStringLiteral("i"), 1)});
    QCOMPARE(graphemeRuns.size(), 1);
    QCOMPARE(graphemeRuns.front().text, QStringLiteral("fi"));
}

void TerminalTextRunsTest::
    isolatesCursorCellsWithoutExtraGraphemesWhenEnabled()
{
    TerminalTextCell cursor = cell(QStringLiteral("b"), 1);
    cursor.cursor = true;
    const std::initializer_list<TerminalTextCell> cells{
        cell(QStringLiteral("a"), 0),
        cursor,
        cell(QStringLiteral("c"), 2),
    };

    const QVector<TerminalTextRun> enabled = plan(cells, true);
    QCOMPARE(enabled.size(), 3);
    QCOMPARE(enabled.at(1).text, QStringLiteral("b"));

    const QVector<TerminalTextRun> disabled = plan(cells, false);
    QCOMPARE(disabled.size(), 1);
    QCOMPARE(disabled.front().text, QStringLiteral("abc"));

    cursor.plainCodepoint = false;
    cursor.extendedGrapheme = true;
    QCOMPARE(plan({cell(QStringLiteral("a"), 0), cursor,
                   cell(QStringLiteral("c"), 2)})
                 .size(),
             1);

    TerminalTextCell emptyCursor = cell({}, 1);
    emptyCursor.cursor = true;
    const QVector<TerminalTextRun> aroundEmpty =
        plan({cell(QStringLiteral("a"), 0), emptyCursor,
              cell(QStringLiteral("c"), 2)});
    QCOMPARE(aroundEmpty.size(), 2);
    QCOMPARE(aroundEmpty.at(0).text, QStringLiteral("a"));
    QCOMPARE(aroundEmpty.at(1).text, QStringLiteral("c"));
}

void TerminalTextRunsTest::excludesBackgroundFromTheStyleKey()
{
    TerminalCell left;
    left.foreground = Qt::white;
    left.background = Qt::black;
    left.backgroundExplicit = false;
    TerminalCell right = left;
    right.background = Qt::red;
    right.backgroundExplicit = true;

    QCOMPARE(terminalShapingStyle(left), terminalShapingStyle(right));
}

QTEST_MAIN(TerminalTextRunsTest)

#include "test_terminal_text_runs.moc"
