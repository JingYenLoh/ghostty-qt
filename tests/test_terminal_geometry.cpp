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
    void appliesAsymmetricPointPaddingAtFractionalDpr();
    void balancesPhysicalRemainders_data();
    void balancesPhysicalRemainders();
    void mapsHalfOpenGridCoordinates();
    void survivesExcessivePadding();
};

void TerminalGeometryTest::convertsLogicalViewportToDeviceGeometry()
{
    const std::optional<TerminalSessionGeometry> geometry =
        terminalSessionGeometryForViewport(431.75, 207.25, 10.0, 17.0, 1.5);
    QVERIFY(geometry.has_value());
    QCOMPARE(*geometry,
             TerminalSessionGeometry({
                 .columns = 43,
                 .rows = 11,
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
        << 100.0 << 100.0 << 8.0 << std::numeric_limits<double>::quiet_NaN()
        << 1.0;
    QTest::newRow("infinite-scale") << 100.0 << 100.0 << 8.0 << 16.0
                                    << std::numeric_limits<double>::infinity();
}

void TerminalGeometryTest::rejectsInvalidViewport()
{
    QFETCH(double, width);
    QFETCH(double, height);
    QFETCH(double, cellWidth);
    QFETCH(double, cellHeight);
    QFETCH(double, devicePixelRatio);
    QVERIFY(!terminalSessionGeometryForViewport(width, height, cellWidth,
                                                cellHeight, devicePixelRatio)
                 .has_value());
}

void TerminalGeometryTest::saturatesExtremeValues()
{
    const std::optional<TerminalSessionGeometry> geometry =
        terminalSessionGeometryForViewport(std::numeric_limits<double>::max(),
                                           std::numeric_limits<double>::max(),
                                           1.0, 1.0,
                                           std::numeric_limits<double>::max());
    QVERIFY(geometry.has_value());
    QCOMPARE(geometry->columns, 1);
    QCOMPARE(geometry->rows, 1);
    QCOMPARE(geometry->cellWidthPixels, std::numeric_limits<int>::max());
    QCOMPARE(geometry->cellHeightPixels, std::numeric_limits<int>::max());
    QCOMPARE(geometry->surfaceWidthPixels, std::numeric_limits<int>::max());
    QCOMPARE(geometry->surfaceHeightPixels, std::numeric_limits<int>::max());
}

void TerminalGeometryTest::normalizesWorkerFacingGeometry()
{
    const TerminalSessionGeometry geometry = normalizedTerminalSessionGeometry({
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

void TerminalGeometryTest::appliesAsymmetricPointPaddingAtFractionalDpr()
{
    const auto layout = terminalViewportLayout({
        .surfaceSize = QSizeF(101, 62),
        .cellSize = QSizeF(8, 10),
        .devicePixelRatio = 1.25,
        .padding =
            {
                .horizontal = {.leadingPoints = 1, .trailingPoints = 2},
                .vertical = {.leadingPoints = 4, .trailingPoints = 5},
            },
    });
    QVERIFY(layout.has_value());
    QCOMPARE(layout->session.surfaceWidthPixels, 126);
    QCOMPARE(layout->session.surfaceHeightPixels, 78);
    QCOMPARE(layout->session.cellWidthPixels, 10);
    QCOMPARE(layout->session.cellHeightPixels, 13);
    QCOMPARE(layout->session.columns, 12);
    QCOMPARE(layout->session.rows, 4);
    QCOMPARE(layout->session.padding,
             TerminalSessionGeometry::Padding(
                 {.top = 6, .right = 3, .bottom = 8, .left = 1}));
    QCOMPARE(layout->session.terminalWidthPixels(), 122);
    QCOMPARE(layout->session.terminalHeightPixels(), 64);
    QCOMPARE(layout->gridRect, QRectF(0.8, 4.8, 96, 41.6));

    const QMarginsF margins = terminalExplicitPaddingMargins(
        {
            .horizontal = {.leadingPoints = 1, .trailingPoints = 2},
            .vertical = {.leadingPoints = 4, .trailingPoints = 5},
        },
        1.25);
    QCOMPARE(margins, QMarginsF(0.8, 4.8, 2.4, 6.4));
}

void TerminalGeometryTest::balancesPhysicalRemainders_data()
{
    QTest::addColumn<int>("modeValue");
    QTest::addColumn<int>("top");
    QTest::addColumn<int>("right");
    QTest::addColumn<int>("bottom");
    QTest::addColumn<int>("left");

    QTest::newRow("disabled")
        << static_cast<int>(TerminalPaddingBalance::Disabled) << 4 << 4 << 4
        << 4;
    QTest::newRow("balanced")
        << static_cast<int>(TerminalPaddingBalance::Balanced) << 9 << 7 << 15
        << 7;
    QTest::newRow("equal") << static_cast<int>(TerminalPaddingBalance::Equal)
                           << 12 << 7 << 12 << 7;
}

void TerminalGeometryTest::balancesPhysicalRemainders()
{
    QFETCH(int, modeValue);
    QFETCH(int, top);
    QFETCH(int, right);
    QFETCH(int, bottom);
    QFETCH(int, left);
    const TerminalSessionGeometry::Padding expectedPadding{
        .top = top,
        .right = right,
        .bottom = bottom,
        .left = left,
    };

    const auto layout = terminalViewportLayout({
        .surfaceSize = QSizeF(105, 85),
        .cellSize = QSizeF(10, 20),
        .devicePixelRatio = 1.0,
        .padding =
            {
                .horizontal = {.leadingPoints = 3, .trailingPoints = 3},
                .vertical = {.leadingPoints = 3, .trailingPoints = 3},
                .balance = static_cast<TerminalPaddingBalance>(modeValue),
            },
    });
    QVERIFY(layout.has_value());
    QCOMPARE(layout->session.columns, 9);
    QCOMPARE(layout->session.rows, 3);
    QCOMPARE(layout->session.padding, expectedPadding);
    QCOMPARE(layout->gridRect.topLeft(),
             QPointF(expectedPadding.left, expectedPadding.top));
    QCOMPARE(layout->gridRect.size(), QSizeF(90, 60));
}

void TerminalGeometryTest::mapsHalfOpenGridCoordinates()
{
    const auto layout = terminalViewportLayout({
        .surfaceSize = QSizeF(100, 80),
        .cellSize = QSizeF(10, 20),
        .devicePixelRatio = 1.0,
        .padding =
            {
                .horizontal = {.leadingPoints = 3, .trailingPoints = 3},
                .vertical = {.leadingPoints = 3, .trailingPoints = 3},
            },
    });
    QVERIFY(layout.has_value());
    QCOMPARE(layout->gridRect, QRectF(4, 4, 90, 60));

    QCOMPARE(layout->strictCellAt(layout->gridRect.topLeft()),
             std::optional(QPoint(0, 0)));
    QCOMPARE(
        layout->strictCellAt(QPointF(
            std::nextafter(layout->gridRect.right(), layout->gridRect.left()),
            std::nextafter(layout->gridRect.bottom(), layout->gridRect.top()))),
        std::optional(QPoint(8, 2)));
    QVERIFY(!layout
                 ->strictCellAt(
                     QPointF(layout->gridRect.right(), layout->gridRect.top()))
                 .has_value());
    QVERIFY(!layout
                 ->strictCellAt(QPointF(layout->gridRect.left(),
                                        layout->gridRect.bottom()))
                 .has_value());
    QVERIFY(!layout->strictCellAt(QPointF(3.999, 4)).has_value());
    QVERIFY(!layout->strictCellAt(QPointF(4, 3.999)).has_value());

    QCOMPARE(layout->clampedCellAt(QPointF(3.999, 3.999)), QPoint(0, 0));
    QCOMPARE(layout->clampedCellAt(
                 QPointF(layout->gridRect.right(), layout->gridRect.bottom())),
             QPoint(8, 2));
    QCOMPARE(
        layout->clampedCellAt(QPointF(std::numeric_limits<qreal>::infinity(),
                                      std::numeric_limits<qreal>::infinity())),
        QPoint(8, 2));
    QCOMPARE(
        layout->clampedCellAt(QPointF(std::numeric_limits<qreal>::quiet_NaN(),
                                      std::numeric_limits<qreal>::quiet_NaN())),
        QPoint(0, 0));
    QVERIFY(
        !layout
             ->strictCellAt(QPointF(std::numeric_limits<qreal>::quiet_NaN(), 4))
             .has_value());
}

void TerminalGeometryTest::survivesExcessivePadding()
{
    const auto layout = terminalViewportLayout({
        .surfaceSize = QSizeF(20, 20),
        .cellSize = QSizeF(8, 10),
        .devicePixelRatio = 1.0,
        .padding =
            {
                .horizontal = {.leadingPoints = 100, .trailingPoints = 100},
                .vertical = {.leadingPoints = 100, .trailingPoints = 100},
            },
    });
    QVERIFY(layout.has_value());
    QCOMPARE(layout->session.columns, 1);
    QCOMPARE(layout->session.rows, 1);
    QCOMPARE(layout->session.terminalWidthPixels(), 0);
    QCOMPARE(layout->session.terminalHeightPixels(), 0);
    QCOMPARE(layout->clampedCellAt(QPointF(-100, -100)), QPoint(0, 0));
    QCOMPARE(layout->clampedCellAt(QPointF(100, 100)), QPoint(0, 0));
    QVERIFY(!layout->strictCellAt(QPointF(-100, -100)).has_value());
}

QTEST_GUILESS_MAIN(TerminalGeometryTest)

#include "test_terminal_geometry.moc"
