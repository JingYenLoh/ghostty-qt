#include "terminal_geometry.h"

#include <QTest>

#include <limits>

class TerminalGeometryTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void convertsLogicalViewportToDeviceGeometry();
    void rejectsInvalidViewport_data();
    void rejectsInvalidViewport();
    void saturatesExtremeValues();
    void normalizesWorkerFacingGeometry();
};

void TerminalGeometryTest::convertsLogicalViewportToDeviceGeometry()
{
    const std::optional<TerminalSessionGeometry> geometry =
        terminalSessionGeometryForViewport(
            431.75, 207.25, 10.0, 17.0, 1.5);
    QVERIFY(geometry.has_value());
    QCOMPARE(*geometry, TerminalSessionGeometry({
        .columns = 43,
        .rows = 12,
        .cellWidthPixels = 15,
        .cellHeightPixels = 26,
        .surfaceWidthPixels = 648,
        .surfaceHeightPixels = 311,
    }));
}

void TerminalGeometryTest::rejectsInvalidViewport_data()
{
    QTest::addColumn<double>("width");
    QTest::addColumn<double>("height");
    QTest::addColumn<double>("cellWidth");
    QTest::addColumn<double>("cellHeight");
    QTest::addColumn<double>("devicePixelRatio");

    QTest::newRow("zero-width") << 0.0 << 100.0 << 8.0 << 16.0 << 1.0;
    QTest::newRow("negative-height") << 100.0 << -1.0 << 8.0 << 16.0 << 1.0;
    QTest::newRow("zero-cell-width") << 100.0 << 100.0 << 0.0 << 16.0 << 1.0;
    QTest::newRow("nan-cell-height")
        << 100.0 << 100.0 << 8.0
        << std::numeric_limits<double>::quiet_NaN() << 1.0;
    QTest::newRow("infinite-scale")
        << 100.0 << 100.0 << 8.0 << 16.0
        << std::numeric_limits<double>::infinity();
}

void TerminalGeometryTest::rejectsInvalidViewport()
{
    QFETCH(double, width);
    QFETCH(double, height);
    QFETCH(double, cellWidth);
    QFETCH(double, cellHeight);
    QFETCH(double, devicePixelRatio);
    QVERIFY(!terminalSessionGeometryForViewport(
                 width, height, cellWidth, cellHeight, devicePixelRatio)
                 .has_value());
}

void TerminalGeometryTest::saturatesExtremeValues()
{
    const std::optional<TerminalSessionGeometry> geometry =
        terminalSessionGeometryForViewport(
            std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max(),
            1.0, 1.0, std::numeric_limits<double>::max());
    QVERIFY(geometry.has_value());
    QCOMPARE(geometry->columns,
             static_cast<int>(std::numeric_limits<quint16>::max()));
    QCOMPARE(geometry->rows,
             static_cast<int>(std::numeric_limits<quint16>::max()));
    QCOMPARE(geometry->cellWidthPixels, std::numeric_limits<int>::max());
    QCOMPARE(geometry->cellHeightPixels, std::numeric_limits<int>::max());
    QCOMPARE(geometry->surfaceWidthPixels, std::numeric_limits<int>::max());
    QCOMPARE(geometry->surfaceHeightPixels, std::numeric_limits<int>::max());
}

void TerminalGeometryTest::normalizesWorkerFacingGeometry()
{
    const TerminalSessionGeometry geometry =
        normalizedTerminalSessionGeometry({
            .columns = -1,
            .rows = std::numeric_limits<int>::max(),
            .cellWidthPixels = 0,
            .cellHeightPixels = -10,
            .surfaceWidthPixels = 0,
            .surfaceHeightPixels = -1,
        });
    QCOMPARE(geometry.columns, 1);
    QCOMPARE(geometry.rows,
             static_cast<int>(std::numeric_limits<quint16>::max()));
    QCOMPARE(geometry.cellWidthPixels, 1);
    QCOMPARE(geometry.cellHeightPixels, 1);
    QCOMPARE(geometry.surfaceWidthPixels, 1);
    QCOMPARE(geometry.surfaceHeightPixels, 1);
}

QTEST_GUILESS_MAIN(TerminalGeometryTest)

#include "test_terminal_geometry.moc"
