#include "terminal_glyph_plan.h"

#include <QFontDatabase>
#include <QGlyphRun>
#include <QTest>
#include <QTextLayout>
#include <QTextOption>

#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace {

TerminalTextRun textRun(QString text, QFont font)
{
    TerminalTextRun run;
    run.text = std::move(text);
    run.font = std::move(font);
    run.columnSpan = run.text.size();
    for (int index = 0; index < run.text.size(); ++index) {
        run.boundaries.append({
            .textPosition = index + 1,
            .column = index + 1,
        });
    }
    return run;
}

struct ShapedLine {
    explicit ShapedLine(const TerminalTextRun &run,
                        Qt::LayoutDirection direction = Qt::LeftToRight,
                        bool underline = false)
        : layout(std::make_unique<QTextLayout>(run.text, run.font))
    {
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        option.setFlags(QTextOption::IncludeTrailingSpaces);
        option.setTextDirection(direction);
        layout->setTextOption(option);
        if (underline) {
            QTextLayout::FormatRange range;
            range.start = 0;
            range.length = run.text.size();
            range.format.setFontUnderline(true);
            layout->setFormats({range});
        }

        layout->beginLayout();
        line = layout->createLine();
        if (line.isValid()) {
            line.setLineWidth(1'000.0);
            line.setPosition({0.0, 7.5});
        }
        layout->endLayout();
    }

    std::unique_ptr<QTextLayout> layout;
    QTextLine line;
};

TerminalGlyphPlan qtGlyphs(const QTextLine &line, const QPointF &origin)
{
    constexpr QTextLayout::GlyphRunRetrievalFlags flags =
        QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveGlyphPositions;
    TerminalGlyphPlan result;
    for (const QGlyphRun &glyphRun : line.glyphRuns(-1, -1, flags)) {
        const QList<quint32> glyphIndexes = glyphRun.glyphIndexes();
        const QList<QPointF> positions = glyphRun.positions();
        for (qsizetype index = 0; index < glyphIndexes.size(); ++index) {
            result.append({
                .font = glyphRun.rawFont(),
                .baselinePosition = origin + positions.at(index),
                .glyphIndex = glyphIndexes.at(index),
            });
        }
    }
    return result;
}

} // namespace

class TerminalGlyphPlanTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void acceptsExactPlainAsciiWithoutReshaping();
    void rejectsLigatures();
    void rejectsWideAndNonAsciiCells();
    void rejectsMalformedInputs();
    void rejectsDecoratedGlyphRuns();

private:
    [[nodiscard]] QFont font() const;

    int fontId_ = -1;
    QString family_;
};

void TerminalGlyphPlanTest::initTestCase()
{
    const QString path =
        QFINDTESTDATA("../ghostty/src/font/res/Inconsolata-Regular.ttf");
    QVERIFY2(!path.isEmpty(), "Bundled glyph-plan test font was not found");
    fontId_ = QFontDatabase::addApplicationFont(path);
    QVERIFY(fontId_ >= 0);
    family_ = QFontDatabase::applicationFontFamilies(fontId_).value(0);
    QVERIFY(!family_.isEmpty());
}

void TerminalGlyphPlanTest::cleanupTestCase()
{
    QVERIFY(QFontDatabase::removeApplicationFont(fontId_));
}

QFont TerminalGlyphPlanTest::font() const
{
    QFont result(family_);
    result.setPixelSize(32);
    result.setFixedPitch(true);
    result.setHintingPreference(QFont::PreferFullHinting);
    return result;
}

void TerminalGlyphPlanTest::acceptsExactPlainAsciiWithoutReshaping()
{
    const TerminalTextRun run = textRun(QStringLiteral("ASCII 19~"), font());
    const ShapedLine shaped(run);
    QVERIFY(shaped.line.isValid());
    const qreal cellWidth =
        shaped.line.cursorToX(run.text.size()) / run.columnSpan;
    QCOMPARE(terminalTextGridFit(shaped.line, run, cellWidth, 1.0),
             TerminalTextGridFit::Exact);

    const QPointF origin(19.5, 41.25);
    const std::optional<TerminalGlyphPlan> plan =
        terminalGlyphPlan(run, shaped.line, TerminalTextGridFit::Exact, origin);
    QVERIFY(plan.has_value());
    QCOMPARE(*plan, qtGlyphs(shaped.line, origin));
    QCOMPARE(plan->size(), run.text.size());
    for (const TerminalGlyphInstance &glyph : *plan) {
        QVERIFY(glyph.font.isValid());
        QVERIFY(std::isfinite(glyph.baselinePosition.x()));
        QVERIFY(std::isfinite(glyph.baselinePosition.y()));
    }
}

void TerminalGlyphPlanTest::rejectsLigatures()
{
    QFont ligatureFont = font();
    ligatureFont.setFeature(QFont::Tag("dlig"), 1);
    const TerminalTextRun run =
        textRun(QStringLiteral(">="), std::move(ligatureFont));
    const ShapedLine shaped(run);
    QVERIFY(shaped.line.isValid());

    constexpr QTextLayout::GlyphRunRetrievalFlags flags =
        QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveStringIndexes;
    qsizetype glyphCount = 0;
    for (const QGlyphRun &glyphRun : shaped.line.glyphRuns(-1, -1, flags)) {
        glyphCount += glyphRun.glyphIndexes().size();
    }
    QCOMPARE(glyphCount, qsizetype{1});
    QVERIFY(!terminalGlyphPlan(run, shaped.line, TerminalTextGridFit::Exact, {})
                 .has_value());
}

void TerminalGlyphPlanTest::rejectsWideAndNonAsciiCells()
{
    TerminalTextRun wide = textRun(QStringLiteral("W"), font());
    wide.columnSpan = 2;
    wide.boundaries.front().column = 2;
    const ShapedLine wideLine(wide);
    QVERIFY(
        !terminalGlyphPlan(wide, wideLine.line, TerminalTextGridFit::Exact, {})
             .has_value());

    const TerminalTextRun nonAscii = textRun(QStringLiteral("café"), font());
    const ShapedLine nonAsciiLine(nonAscii);
    QVERIFY(!terminalGlyphPlan(nonAscii, nonAsciiLine.line,
                               TerminalTextGridFit::Exact, {})
                 .has_value());
}

void TerminalGlyphPlanTest::rejectsMalformedInputs()
{
    const TerminalTextRun valid = textRun(QStringLiteral("abc"), font());
    const ShapedLine shaped(valid);

    TerminalTextRun malformed = valid;
    malformed.boundaries.removeLast();
    QVERIFY(!terminalGlyphPlan(malformed, shaped.line,
                               TerminalTextGridFit::Exact, {})
                 .has_value());

    malformed = valid;
    malformed.boundaries[1].textPosition = 1;
    QVERIFY(!terminalGlyphPlan(malformed, shaped.line,
                               TerminalTextGridFit::Exact, {})
                 .has_value());

    TerminalTextRun proportional = valid;
    proportional.font.setFixedPitch(false);
    const ShapedLine proportionalLine(proportional);
    QVERIFY(!terminalGlyphPlan(proportional, proportionalLine.line,
                               TerminalTextGridFit::Exact, {})
                 .has_value());

    QVERIFY(!terminalGlyphPlan(valid, shaped.line,
                               TerminalTextGridFit::ShapedClusters, {})
                 .has_value());
    QVERIFY(!terminalGlyphPlan(valid, shaped.line, TerminalTextGridFit::Exact,
                               {std::numeric_limits<qreal>::infinity(), 0.0})
                 .has_value());
}

void TerminalGlyphPlanTest::rejectsDecoratedGlyphRuns()
{
    const TerminalTextRun run = textRun(QStringLiteral("abc"), font());
    const ShapedLine decorated(run, Qt::LeftToRight, true);
    QVERIFY(
        !terminalGlyphPlan(run, decorated.line, TerminalTextGridFit::Exact, {})
             .has_value());
}

QTEST_MAIN(TerminalGlyphPlanTest)

#include "test_terminal_glyph_plan.moc"
