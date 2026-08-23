#include "desktop/quick_terminal.h"

#include <QTest>

#include <bit>
#include <limits>

class QuickTerminalTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void hasLinuxDefaults();
    void calculatesDefaultSizes_data();
    void calculatesDefaultSizes();
    void calculatesPrimaryPercentageSizes_data();
    void calculatesPrimaryPercentageSizes();
    void calculatesPrimaryPixelSizes_data();
    void calculatesPrimaryPixelSizes();
    void mapsMixedAxes_data();
    void mapsMixedAxes();
    void comparesPercentageBitPatterns();
    void preservesFloat32PercentageSemantics();
    void saturatesInvalidAndOversizedExtents();
    void describesLayerShellPlacementIntent_data();
    void describesLayerShellPlacementIntent();
    void placesEveryPosition_data();
    void placesEveryPosition();
    void preservesOversizedRequestedGeometry();
    void calculatesOptionsGeometry();
};

namespace {

void addSizeRow(const char *name, QuickTerminalPosition position, QSize output,
                QSize expected)
{
    QTest::newRow(name) << static_cast<int>(position) << output << expected;
}

QuickTerminalPosition fetchedPosition(int value)
{
    return static_cast<QuickTerminalPosition>(value);
}

} // namespace

void QuickTerminalTest::hasLinuxDefaults()
{
    const QuickTerminalOptions options;

    QCOMPARE(options.position, QuickTerminalPosition::Top);
    QCOMPARE(options.size, QuickTerminalSize{});
    QCOMPARE(options.screen, QuickTerminalScreen::Main);
    QCOMPARE(options.autohide, false);
    QCOMPARE(options.keyboardInteractivity,
             QuickTerminalKeyboardInteractivity::OnDemand);
}

void QuickTerminalTest::calculatesDefaultSizes_data()
{
    QTest::addColumn<int>("positionValue");
    QTest::addColumn<QSize>("output");
    QTest::addColumn<QSize>("expected");

    constexpr QSize landscape(2560, 1600);
    constexpr QSize portrait(1600, 2560);

    addSizeRow("landscape-top", QuickTerminalPosition::Top, landscape,
               {2560, 400});
    addSizeRow("landscape-bottom", QuickTerminalPosition::Bottom, landscape,
               {2560, 400});
    addSizeRow("landscape-left", QuickTerminalPosition::Left, landscape,
               {400, 1600});
    addSizeRow("landscape-right", QuickTerminalPosition::Right, landscape,
               {400, 1600});
    addSizeRow("landscape-center", QuickTerminalPosition::Center, landscape,
               {800, 400});
    addSizeRow("portrait-top", QuickTerminalPosition::Top, portrait,
               {1600, 400});
    addSizeRow("portrait-bottom", QuickTerminalPosition::Bottom, portrait,
               {1600, 400});
    addSizeRow("portrait-left", QuickTerminalPosition::Left, portrait,
               {400, 2560});
    addSizeRow("portrait-right", QuickTerminalPosition::Right, portrait,
               {400, 2560});
    addSizeRow("portrait-center", QuickTerminalPosition::Center, portrait,
               {400, 800});
    addSizeRow("square-is-landscape", QuickTerminalPosition::Center,
               {1200, 1200}, {800, 400});
}

void QuickTerminalTest::calculatesDefaultSizes()
{
    QFETCH(int, positionValue);
    QFETCH(QSize, output);
    QFETCH(QSize, expected);

    QCOMPARE(quickTerminalSize({}, fetchedPosition(positionValue), output),
             expected);
}

void QuickTerminalTest::calculatesPrimaryPercentageSizes_data()
{
    QTest::addColumn<int>("positionValue");
    QTest::addColumn<QSize>("output");
    QTest::addColumn<QSize>("expected");

    constexpr QSize landscape(2560, 1600);
    addSizeRow("top", QuickTerminalPosition::Top, landscape, {2560, 320});
    addSizeRow("bottom", QuickTerminalPosition::Bottom, landscape, {2560, 320});
    addSizeRow("left", QuickTerminalPosition::Left, landscape, {512, 1600});
    addSizeRow("right", QuickTerminalPosition::Right, landscape, {512, 1600});
    addSizeRow("landscape-center", QuickTerminalPosition::Center, landscape,
               {512, 400});
    addSizeRow("portrait-center", QuickTerminalPosition::Center, {1600, 2560},
               {400, 512});
}

void QuickTerminalTest::calculatesPrimaryPercentageSizes()
{
    QFETCH(int, positionValue);
    QFETCH(QSize, output);
    QFETCH(QSize, expected);
    const QuickTerminalSize size{
        .primary = QuickTerminalPercentage{20.0F},
    };

    QCOMPARE(quickTerminalSize(size, fetchedPosition(positionValue), output),
             expected);
}

void QuickTerminalTest::calculatesPrimaryPixelSizes_data()
{
    QTest::addColumn<int>("positionValue");
    QTest::addColumn<QSize>("output");
    QTest::addColumn<QSize>("expected");

    constexpr QSize landscape(2560, 1600);
    addSizeRow("top", QuickTerminalPosition::Top, landscape, {2560, 600});
    addSizeRow("bottom", QuickTerminalPosition::Bottom, landscape, {2560, 600});
    addSizeRow("left", QuickTerminalPosition::Left, landscape, {600, 1600});
    addSizeRow("right", QuickTerminalPosition::Right, landscape, {600, 1600});
    addSizeRow("landscape-center", QuickTerminalPosition::Center, landscape,
               {600, 400});
    addSizeRow("portrait-center", QuickTerminalPosition::Center, {1600, 2560},
               {400, 600});
}

void QuickTerminalTest::calculatesPrimaryPixelSizes()
{
    QFETCH(int, positionValue);
    QFETCH(QSize, output);
    QFETCH(QSize, expected);
    const QuickTerminalSize size{
        .primary = QuickTerminalPixels{600},
    };

    QCOMPARE(quickTerminalSize(size, fetchedPosition(positionValue), output),
             expected);
}

void QuickTerminalTest::mapsMixedAxes_data()
{
    QTest::addColumn<int>("positionValue");
    QTest::addColumn<QSize>("output");
    QTest::addColumn<QSize>("expected");

    constexpr QSize landscape(2560, 1600);
    addSizeRow("top", QuickTerminalPosition::Top, landscape, {420, 1104});
    addSizeRow("bottom", QuickTerminalPosition::Bottom, landscape, {420, 1104});
    addSizeRow("left", QuickTerminalPosition::Left, landscape, {1766, 420});
    addSizeRow("right", QuickTerminalPosition::Right, landscape, {1766, 420});
    addSizeRow("landscape-center", QuickTerminalPosition::Center, landscape,
               {1766, 420});
    addSizeRow("portrait-center", QuickTerminalPosition::Center, {1600, 2560},
               {420, 1766});
}

void QuickTerminalTest::mapsMixedAxes()
{
    QFETCH(int, positionValue);
    QFETCH(QSize, output);
    QFETCH(QSize, expected);
    const QuickTerminalSize size{
        .primary = QuickTerminalPercentage{69.0F},
        .secondary = QuickTerminalPixels{420},
    };

    QCOMPARE(quickTerminalSize(size, fetchedPosition(positionValue), output),
             expected);
}

void QuickTerminalTest::comparesPercentageBitPatterns()
{
    const QuickTerminalPercentage quietNan{
        std::bit_cast<float>(quint32{0x7fc00000U})};
    const QuickTerminalPercentage sameQuietNan{
        std::bit_cast<float>(quint32{0x7fc00000U})};
    const QuickTerminalPercentage differentNan{
        std::bit_cast<float>(quint32{0x7fc00001U})};

    QVERIFY(quietNan == sameQuietNan);
    QVERIFY(!(quietNan == differentNan));
    QVERIFY(!(QuickTerminalPercentage{0.0F} == QuickTerminalPercentage{-0.0F}));
    QVERIFY(QuickTerminalPercentage{69.0F} == QuickTerminalPercentage{69.0F});

    const QuickTerminalSize first{.primary = quietNan};
    const QuickTerminalSize same{.primary = sameQuietNan};
    const QuickTerminalSize changed{.primary = differentNan};
    QCOMPARE(first, same);
    QVERIFY(!(first == changed));
}

void QuickTerminalTest::preservesFloat32PercentageSemantics()
{
    // f32 evaluates 69 / 100 * 300 to exactly 207. Promoting the stored
    // percentage to double before both operations produces 206.999... and
    // would incorrectly truncate to 206.
    QCOMPARE(quickTerminalExtentPixels(QuickTerminalPercentage{69.0F}, 300),
             207);
    QCOMPARE(
        quickTerminalExtentPixels(QuickTerminalPercentage{33.333332F}, 300),
        99);
}

void QuickTerminalTest::saturatesInvalidAndOversizedExtents()
{
    constexpr int maximum = std::numeric_limits<int>::max();

    QCOMPARE(quickTerminalExtentPixels(
                 QuickTerminalPixels{std::numeric_limits<quint32>::max()}, 100),
             maximum);
    QCOMPARE(
        quickTerminalExtentPixels(
            QuickTerminalPercentage{std::numeric_limits<float>::infinity()},
            maximum),
        maximum);
    QCOMPARE(
        quickTerminalExtentPixels(
            QuickTerminalPercentage{std::numeric_limits<float>::quiet_NaN()},
            maximum),
        0);
    QCOMPARE(
        quickTerminalExtentPixels(QuickTerminalPercentage{-25.0F}, maximum), 0);
    QCOMPARE(quickTerminalExtentPixels(QuickTerminalPercentage{50.0F}, -100),
             0);

    const QuickTerminalSize oversized{
        .primary = QuickTerminalPixels{std::numeric_limits<quint32>::max()},
        .secondary = QuickTerminalPixels{std::numeric_limits<quint32>::max()},
    };
    QCOMPARE(quickTerminalSize(oversized, QuickTerminalPosition::Center,
                               {maximum, maximum}),
             QSize(maximum, maximum));
    QCOMPARE(quickTerminalSize({}, QuickTerminalPosition::Top, {-1, -10}),
             QSize(0, 400));
}

void QuickTerminalTest::describesLayerShellPlacementIntent_data()
{
    QTest::addColumn<int>("positionValue");
    QTest::addColumn<int>("anchorsValue");
    QTest::addColumn<QMargins>("margins");

    QTest::newRow("top") << static_cast<int>(QuickTerminalPosition::Top)
                         << static_cast<int>(Qt::TopEdge)
                         << QMargins(20, 0, 20, 20);
    QTest::newRow("bottom")
        << static_cast<int>(QuickTerminalPosition::Bottom)
        << static_cast<int>(Qt::BottomEdge) << QMargins(20, 20, 20, 0);
    QTest::newRow("left") << static_cast<int>(QuickTerminalPosition::Left)
                          << static_cast<int>(Qt::LeftEdge)
                          << QMargins(0, 20, 20, 20);
    QTest::newRow("right") << static_cast<int>(QuickTerminalPosition::Right)
                           << static_cast<int>(Qt::RightEdge)
                           << QMargins(20, 20, 0, 20);
    QTest::newRow("center") << static_cast<int>(QuickTerminalPosition::Center)
                            << 0 << QMargins(20, 20, 20, 20);
}

void QuickTerminalTest::describesLayerShellPlacementIntent()
{
    QFETCH(int, positionValue);
    QFETCH(int, anchorsValue);
    QFETCH(QMargins, margins);

    const QuickTerminalPlacementIntent intent =
        quickTerminalPlacementIntent(fetchedPosition(positionValue));
    QCOMPARE(static_cast<int>(intent.anchors), anchorsValue);
    QCOMPARE(intent.margins, margins);

    const QuickTerminalPlacementIntent noNegativeMargins =
        quickTerminalPlacementIntent(fetchedPosition(positionValue), -5);
    QCOMPARE(noNegativeMargins.margins, QMargins());
}

void QuickTerminalTest::placesEveryPosition_data()
{
    QTest::addColumn<int>("positionValue");
    QTest::addColumn<QRect>("expected");

    QTest::newRow("top") << static_cast<int>(QuickTerminalPosition::Top)
                         << QRect(450, 50, 300, 200);
    QTest::newRow("bottom") << static_cast<int>(QuickTerminalPosition::Bottom)
                            << QRect(450, 550, 300, 200);
    QTest::newRow("left") << static_cast<int>(QuickTerminalPosition::Left)
                          << QRect(100, 300, 300, 200);
    QTest::newRow("right") << static_cast<int>(QuickTerminalPosition::Right)
                           << QRect(800, 300, 300, 200);
    QTest::newRow("center") << static_cast<int>(QuickTerminalPosition::Center)
                            << QRect(450, 300, 300, 200);
}

void QuickTerminalTest::placesEveryPosition()
{
    QFETCH(int, positionValue);
    QFETCH(QRect, expected);

    QCOMPARE(quickTerminalGeometry(fetchedPosition(positionValue),
                                   QRect(100, 50, 1000, 700), QSize(300, 200)),
             expected);
}

void QuickTerminalTest::preservesOversizedRequestedGeometry()
{
    const QRect output(100, 50, 1000, 700);
    const QSize requested(1200, 800);

    QCOMPARE(
        quickTerminalGeometry(QuickTerminalPosition::Top, output, requested),
        QRect(0, 50, 1200, 800));
    QCOMPARE(
        quickTerminalGeometry(QuickTerminalPosition::Bottom, output, requested),
        QRect(0, -50, 1200, 800));
    QCOMPARE(
        quickTerminalGeometry(QuickTerminalPosition::Left, output, requested),
        QRect(100, 0, 1200, 800));
    QCOMPARE(
        quickTerminalGeometry(QuickTerminalPosition::Right, output, requested),
        QRect(-100, 0, 1200, 800));
    QCOMPARE(
        quickTerminalGeometry(QuickTerminalPosition::Center, output, requested),
        QRect(0, 0, 1200, 800));
}

void QuickTerminalTest::calculatesOptionsGeometry()
{
    const QuickTerminalOptions options{
        .position = QuickTerminalPosition::Bottom,
        .size =
            {
                .primary = QuickTerminalPercentage{25.0F},
            },
    };

    QCOMPARE(quickTerminalGeometry(options, QRect(20, 30, 2560, 1600)),
             QRect(20, 1230, 2560, 400));
}

QTEST_APPLESS_MAIN(QuickTerminalTest)

#include "test_quick_terminal.moc"
