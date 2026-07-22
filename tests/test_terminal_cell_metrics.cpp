#include "terminal_cell_metrics.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QTest>

#include <algorithm>
#include <cmath>

namespace {

void verifyIntegralMetrics(const TerminalCellMetrics &actual)
{
    const QFontMetricsF expected(actual.font);
    QCOMPARE(actual.cellWidth,
             std::max<qreal>(
                 1.0,
                 std::ceil(expected.horizontalAdvance(QLatin1Char('M')))));
    QCOMPARE(actual.cellHeight,
             std::max<qreal>(1.0, std::ceil(expected.height())));
    QCOMPARE(actual.baseline,
             std::ceil(expected.ascent()
                       + (actual.cellHeight - expected.height()) / 2.0));

    QVERIFY(actual.cellWidth >= 1.0);
    QVERIFY(actual.cellHeight >= 1.0);
    QVERIFY(std::isfinite(actual.cellWidth));
    QVERIFY(std::isfinite(actual.cellHeight));
    QVERIFY(std::isfinite(actual.baseline));
    QCOMPARE(actual.cellWidth, std::trunc(actual.cellWidth));
    QCOMPARE(actual.cellHeight, std::trunc(actual.cellHeight));
    QCOMPARE(actual.baseline, std::trunc(actual.baseline));
}

} // namespace

class TerminalCellMetricsTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void selectsConfiguredFont();
    void selectsSystemFixedFontForEmptyFamily();
    void preservesExistingFont();
};

void TerminalCellMetricsTest::selectsConfiguredFont()
{
    const QString family = QFontDatabase::systemFont(
        QFontDatabase::FixedFont).family();
    const TerminalCellMetrics actual = terminalCellMetrics(family, 15.5);

    QCOMPARE(resolveTerminalFontFamily(family), family);
    QCOMPARE(actual.font.family(), family);
    QCOMPARE(actual.font.pointSizeF(), 15.5);
    QVERIFY(actual.font.fixedPitch());
    QCOMPARE(actual.font.styleHint(), QFont::Monospace);
    verifyIntegralMetrics(actual);
}

void TerminalCellMetricsTest::selectsSystemFixedFontForEmptyFamily()
{
    const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const TerminalCellMetrics actual = terminalCellMetrics(QString(), 12.0);

    QCOMPARE(resolveTerminalFontFamily(QString()), fixedFont.family());
    QCOMPARE(actual.font.family(), fixedFont.family());
    QCOMPARE(actual.font.pointSizeF(), 12.0);
    QVERIFY(actual.font.fixedPitch());
    QCOMPARE(actual.font.styleHint(), QFont::Monospace);
    verifyIntegralMetrics(actual);
}

void TerminalCellMetricsTest::preservesExistingFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(9.75);
    font.setItalic(true);

    const TerminalCellMetrics actual = terminalCellMetrics(font);

    QCOMPARE(actual.font, font);
    verifyIntegralMetrics(actual);
}

QTEST_MAIN(TerminalCellMetricsTest)

#include "test_terminal_cell_metrics.moc"
