#include "terminal_text_grid_fit.h"
#include "terminal_text_runs.h"

#include <QFontDatabase>
#include <QGlyphRun>
#include <QTest>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
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

struct ShapingMetadata {
    qsizetype glyphCount = 0;
    QVector<int> clusterStarts;
};

ShapingMetadata shapingMetadata(const QTextLine &line)
{
    ShapingMetadata result;
    const QList<QGlyphRun> glyphRuns = line.glyphRuns(
        -1, -1,
        QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveStringIndexes);
    for (const QGlyphRun &glyphRun : glyphRuns) {
        result.glyphCount += glyphRun.glyphIndexes().size();
        for (const qsizetype index : glyphRun.stringIndexes()) {
            result.clusterStarts.append(static_cast<int>(index));
        }
    }
    std::ranges::sort(result.clusterStarts);
    const auto uniqueEnd = std::ranges::unique(result.clusterStarts).begin();
    result.clusterStarts.erase(uniqueEnd, result.clusterStarts.end());
    return result;
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
    void acceptsExactGridFit();
    void acceptsInternalLigatureClusterDrift();
    void rejectsExposedLigatureClusterDrift();
    void rejectsMalformedBoundaryMap();
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

    const QVector<TerminalTextRun> programmingRuns =
        plan({cell(QStringLiteral("!"), 0), cell(QStringLiteral("="), 1),
              cell(QStringLiteral(">"), 2), cell(QStringLiteral("="), 3),
              cell(QStringLiteral("="), 4), cell(QStringLiteral("-"), 5),
              cell(QStringLiteral(">"), 6)});
    QCOMPARE(programmingRuns.size(), 1);
    QCOMPARE(programmingRuns.front().text, QStringLiteral("!=>==->"));
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

void TerminalTextRunsTest::acceptsExactGridFit()
{
    TerminalTextRun run{
        .text = QStringLiteral("abc"),
        .columnSpan = 3,
        .boundaries =
            {
                {.textPosition = 1, .column = 1},
                {.textPosition = 2, .column = 2},
                {.textPosition = 3, .column = 3},
            },
    };
    run.font = cell(QStringLiteral("a"), 0).font;
    run.font.setPixelSize(40);

    QTextLayout layout(run.text, run.font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTextDirection(Qt::LeftToRight);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    QVERIFY(line.isValid());
    line.setLineWidth(1'000.0);
    layout.endLayout();

    const qreal totalAdvance = line.cursorToX(run.text.size());
    QVERIFY(totalAdvance > 0.0);
    QCOMPARE(terminalTextGridFit(line, run, totalAdvance / 3.0, 1.0),
             TerminalTextGridFit::Exact);
}

void TerminalTextRunsTest::acceptsInternalLigatureClusterDrift()
{
    const QString path =
        QFINDTESTDATA("../ghostty/src/font/res/Inconsolata-Regular.ttf");
    QVERIFY2(!path.isEmpty(), "Bundled ligature test font was not found");
    const int fontId = QFontDatabase::addApplicationFont(path);
    QVERIFY(fontId >= 0);
    const QString family =
        QFontDatabase::applicationFontFamilies(fontId).value(0);
    QVERIFY(!family.isEmpty());

    QFont font(family);
    font.setPixelSize(40);
    font.setFixedPitch(true);
    font.setFeature(QFont::Tag("dlig"), 1);
    TerminalTextRun run{
        .text = QStringLiteral(">="),
        .font = font,
        .columnSpan = 4,
        .boundaries =
            {
                {.textPosition = 1, .column = 1},
                {.textPosition = 2, .column = 4},
            },
    };

    QTextLayout layout(run.text, run.font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTextDirection(Qt::LeftToRight);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    QVERIFY(line.isValid());
    line.setLineWidth(1'000.0);
    layout.endLayout();

    const qreal totalAdvance = line.cursorToX(run.text.size());
    QVERIFY(totalAdvance > 0.0);
    const ShapingMetadata enabled = shapingMetadata(line);
    QCOMPARE(enabled.glyphCount, qsizetype{1});
    QCOMPARE(enabled.clusterStarts, QVector<int>({0}));
    QCOMPARE(terminalTextGridFit(line, run, totalAdvance / 4.0, 1.0),
             TerminalTextGridFit::ShapedClusters);
    QCOMPARE(terminalTextGridFit(line, run, totalAdvance / 4.0, 1.25),
             TerminalTextGridFit::ShapedClusters);

    QFont disabledFont = font;
    disabledFont.setFeature(QFont::Tag("dlig"), 0);
    QTextLayout disabledLayout(run.text, disabledFont);
    disabledLayout.setTextOption(option);
    disabledLayout.beginLayout();
    QTextLine disabledLine = disabledLayout.createLine();
    QVERIFY(disabledLine.isValid());
    disabledLine.setLineWidth(1'000.0);
    disabledLayout.endLayout();
    const ShapingMetadata disabled = shapingMetadata(disabledLine);
    QCOMPARE(disabled.glyphCount, qsizetype{2});
    QCOMPARE(disabled.clusterStarts, QVector<int>({0, 1}));
    const qreal disabledAdvance = disabledLine.cursorToX(run.text.size());
    QCOMPARE(terminalTextGridFit(disabledLine, run, disabledAdvance / 4.0, 1.0),
             TerminalTextGridFit::Rejected);
    QVERIFY(QFontDatabase::removeApplicationFont(fontId));
}

void TerminalTextRunsTest::rejectsExposedLigatureClusterDrift()
{
    const QString path =
        QFINDTESTDATA("../ghostty/src/font/res/Inconsolata-Regular.ttf");
    QVERIFY2(!path.isEmpty(), "Bundled ligature test font was not found");
    const int fontId = QFontDatabase::addApplicationFont(path);
    QVERIFY(fontId >= 0);
    const QString family =
        QFontDatabase::applicationFontFamilies(fontId).value(0);
    QVERIFY(!family.isEmpty());

    QFont font(family);
    font.setPixelSize(40);
    font.setFixedPitch(true);
    font.setFeature(QFont::Tag("dlig"), 1);
    TerminalTextRun run{
        .text = QStringLiteral(">=x"),
        .font = font,
        .columnSpan = 6,
        .boundaries =
            {
                {.textPosition = 1, .column = 1},
                {.textPosition = 2, .column = 5},
                {.textPosition = 3, .column = 6},
            },
    };

    QTextLayout layout(run.text, run.font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTextDirection(Qt::LeftToRight);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    QVERIFY(line.isValid());
    line.setLineWidth(1'000.0);
    layout.endLayout();

    const qreal totalAdvance = line.cursorToX(run.text.size());
    QVERIFY(totalAdvance > 0.0);
    QCOMPARE(terminalTextGridFit(line, run, totalAdvance / 6.0, 1.0),
             TerminalTextGridFit::Rejected);
    QVERIFY(QFontDatabase::removeApplicationFont(fontId));
}

void TerminalTextRunsTest::rejectsMalformedBoundaryMap()
{
    TerminalTextRun run{
        .text = QStringLiteral("ab"),
        .columnSpan = 2,
        .boundaries =
            {
                {.textPosition = 1, .column = 1},
                {.textPosition = 1, .column = 2},
            },
    };
    run.font = cell(QStringLiteral("a"), 0).font;

    QTextLayout layout(run.text, run.font);
    layout.beginLayout();
    const QTextLine line = layout.createLine();
    QVERIFY(line.isValid());
    layout.endLayout();

    QCOMPARE(terminalTextGridFit(line, run, 10.0, 1.0),
             TerminalTextGridFit::Rejected);
}

QTEST_MAIN(TerminalTextRunsTest)

#include "test_terminal_text_runs.moc"
