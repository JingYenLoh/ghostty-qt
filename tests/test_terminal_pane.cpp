#include "launch_options.h"
#include "terminal_cell_metrics.h"
#include "terminal_clipboard.h"
#include "terminal_controller.h"
#include "terminal_geometry.h"
#include "terminal_pane.h"
#include "terminal_pane_render_probe_p.h"
#include "terminal_types.h"

#include <QColor>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QHoverEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <utility>

namespace {

QString frameText(const TerminalFrame &frame)
{
    QString text;
    text.reserve(frame.cells.size());
    for (const TerminalCell &cell : frame.cells) {
        text.append(cell.text);
    }
    return text;
}

bool updatesContain(const QSignalSpy &spy, const QString &needle)
{
    TerminalFrame frame;
    for (const QList<QVariant> &arguments : spy) {
        const bool applied = applyTerminalUpdate(
            frame, qvariant_cast<TerminalUpdate>(arguments.constFirst()));
        Q_ASSERT(applied);
    }
    return frameText(frame).contains(needle);
}

TerminalFrame accumulatedFrame(const QSignalSpy &spy)
{
    TerminalFrame frame;
    for (const QList<QVariant> &arguments : spy) {
        const bool applied = applyTerminalUpdate(
            frame, qvariant_cast<TerminalUpdate>(arguments.constFirst()));
        Q_ASSERT(applied);
    }
    return frame;
}

bool approximatelyEqual(const QColor &left, const QColor &right)
{
    constexpr int tolerance = 2;
    return std::abs(left.red() - right.red()) <= tolerance
        && std::abs(left.green() - right.green()) <= tolerance
        && std::abs(left.blue() - right.blue()) <= tolerance;
}

QColor sourceOver(const QColor &underlying, const QColor &fill, double alpha)
{
    const auto channel = [alpha](int under, int over) {
        return qRound(static_cast<double>(under) * (1.0 - alpha)
                      + static_cast<double>(over) * alpha);
    };
    return QColor(channel(underlying.red(), fill.red()),
                  channel(underlying.green(), fill.green()),
                  channel(underlying.blue(), fill.blue()));
}

QColor itemPixel(const QQuickWindow &window, const QQuickItem &item,
                 const QImage &image, const QPointF &position)
{
    const QPointF scene = item.mapToScene(position);
    const qreal xScale = static_cast<qreal>(image.width()) / window.width();
    const qreal yScale = static_cast<qreal>(image.height()) / window.height();
    const int x = std::clamp(
        static_cast<int>(std::floor(scene.x() * xScale)),
        0, image.width() - 1);
    const int y = std::clamp(
        static_cast<int>(std::floor(scene.y() * yScale)),
        0, image.height() - 1);
    return image.pixelColor(x, y);
}

bool allVisibleRowsRebuilt(
    const TerminalPaneRenderProbeSnapshot &before,
    const TerminalPaneRenderProbeSnapshot &after)
{
    if (after.rowBuildCounts.isEmpty()) {
        return false;
    }
    for (qsizetype row = 0; row < after.rowBuildCounts.size(); ++row) {
        const quint64 previous = row < before.rowBuildCounts.size()
            ? before.rowBuildCounts.at(row) : 0;
        if (after.rowBuildCounts.at(row) <= previous) {
            return false;
        }
    }
    return true;
}

bool spyContainsBool(const QSignalSpy &spy, bool value)
{
    return std::any_of(spy.cbegin(), spy.cend(), [value](const auto &arguments) {
        return !arguments.isEmpty() && arguments.constFirst().toBool() == value;
    });
}

QBitArray cellMask(int columns, int rows,
                   std::initializer_list<QPoint> cells)
{
    QBitArray mask(static_cast<qsizetype>(columns) * rows);
    for (const QPoint &cell : cells) {
        if (cell.x() >= 0 && cell.x() < columns
            && cell.y() >= 0 && cell.y() < rows) {
            mask.setBit(static_cast<qsizetype>(cell.y()) * columns + cell.x());
        }
    }
    return mask;
}

class ShellEnvironment final {
public:
    ShellEnvironment()
        : wasSet_(qEnvironmentVariableIsSet("SHELL"))
        , previous_(qgetenv("SHELL"))
    {
        qputenv("SHELL", QByteArrayLiteral("/bin/sh"));
    }

    ~ShellEnvironment()
    {
        if (wasSet_) {
            qputenv("SHELL", previous_);
        } else {
            qunsetenv("SHELL");
        }
    }

private:
    bool wasSet_ = false;
    QByteArray previous_;
};

} // namespace

class TerminalPaneTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void resizeOverlayPositions_data();
    void resizeOverlayPositions();
    void resizeOverlayCoalescesAndRestarts();
    void replacesStartingFrameInsteadOfAccumulatingSceneRoots();
    void reloadsFontWithoutOverwritingManualZoom();
    void executesTypedFontSizeActions();
    void packagesInputMethodLifecycleAsOneWorkerRequest();
    void writesClipboardDestinations();
    void copiesRawEffectiveSurfaceTitle();
    void reloadsMiddleClickClipboardPolicy();
    void togglesMouseReportingPolicyAcrossGesturesAndReloads();
    void routesAllPasteEntryPointsThroughController();
    void routesUnsafePasteConfirmationThroughWorker();
    void runsCursorBlinkTimerOnlyWhenNeeded();
    void retainsMainTextRowsAcrossIncrementalUpdates();
    void retainsTextWhileDimmingUnfocusedSplits();
    void rebuildsMainTextRowsAfterWindowChange();
    void rendersConfiguredCellCursorAndDecorationAppearance();
    void routesEmergencyTabShortcuts();
    void routesConfiguredBindingsAndDisablesEmergencyFallback();
    void routesBroadConfiguredActionEffects();
    void routesTypedCloseTabModes();
    void routesViewportAndSelectionActions();
    void routesTerminalControlActions();
    void routesSearchActionsAndRetainsUiState();
    void interactsWithOsc8Hyperlinks();
    void interactsWithRegexLinksAndReloadsLinkUrl();
    void previewsLinksAccordingToPolicyAndBoundsDisplay();
    void keepsOsc8InteractionStableAcrossUnrelatedOutput();
    void restoresOsc8HoverAcrossViewportScroll();
    void letsShiftBypassMouseCaptureForHyperlinks();
    void resetPreservesSurfaceTitleAndClearsWorkingDirectory();
    void routesStructuredSequencesAndCancelsThemOnReload();
    void replaysInvalidStructuredSequenceThroughPty();
    void routesNamedKeyTablesAndClearsThemOnReload();
    void rejectsMalformedFrontendActionsWithoutSideEffects();
};

void TerminalPaneTest::resizeOverlayPositions_data()
{
    QTest::addColumn<int>("position");
    QTest::addColumn<QPointF>("expectedTopLeft");

    QTest::newRow("center")
        << static_cast<int>(ResizeOverlayPosition::Center)
        << QPointF(190.0, 130.0);
    QTest::newRow("top-left")
        << static_cast<int>(ResizeOverlayPosition::TopLeft)
        << QPointF(0.0, 0.0);
    QTest::newRow("top-center")
        << static_cast<int>(ResizeOverlayPosition::TopCenter)
        << QPointF(190.0, 0.0);
    QTest::newRow("top-right")
        << static_cast<int>(ResizeOverlayPosition::TopRight)
        << QPointF(380.0, 0.0);
    QTest::newRow("bottom-left")
        << static_cast<int>(ResizeOverlayPosition::BottomLeft)
        << QPointF(0.0, 260.0);
    QTest::newRow("bottom-center")
        << static_cast<int>(ResizeOverlayPosition::BottomCenter)
        << QPointF(190.0, 260.0);
    QTest::newRow("bottom-right")
        << static_cast<int>(ResizeOverlayPosition::BottomRight)
        << QPointF(380.0, 260.0);
}

void TerminalPaneTest::resizeOverlayPositions()
{
    QFETCH(int, position);
    QFETCH(QPointF, expectedTopLeft);

    ShellEnvironment shell;
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.resizeOverlay.position =
        static_cast<ResizeOverlayPosition>(position);

    TerminalPane pane(options);
    pane.setSize(QSizeF(500.0, 300.0));
    const QRectF rect = pane.resizeOverlayRect();
    QCOMPARE(rect.topLeft(), expectedTopLeft);
    QCOMPARE(rect.size(), QSizeF(120.0, 40.0));
}

void TerminalPaneTest::resizeOverlayCoalescesAndRestarts()
{
    ShellEnvironment shell;
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.resizeOverlay = {
        .mode = ResizeOverlayMode::AfterFirst,
        .position = ResizeOverlayPosition::Center,
        .duration = std::chrono::milliseconds(250),
    };

    {
        LaunchOptions alwaysOptions = options;
        alwaysOptions.resizeOverlay.mode = ResizeOverlayMode::Always;
        TerminalPane always(alwaysOptions);
        always.setSize(QSizeF(500.0, 300.0));
        QTRY_VERIFY_WITH_TIMEOUT(always.resizeOverlayVisible(), 1000);
    }
    {
        LaunchOptions neverOptions = options;
        neverOptions.resizeOverlay.mode = ResizeOverlayMode::Never;
        TerminalPane never(neverOptions);
        never.setSize(QSizeF(500.0, 300.0));
        never.setWidth(700.0);
        QCoreApplication::processEvents();
        QVERIFY(!never.resizeOverlayVisible());
    }

    TerminalPane pane(options);
    pane.setSize(QSizeF(500.0, 300.0));
    QCoreApplication::processEvents();
    QVERIFY(!pane.resizeOverlayVisible());
    const TerminalCellMetrics metrics = terminalCellMetrics(
        options.fontFamily, options.fontSize);
    const auto initialGeometry = terminalSessionGeometryForViewport(
        pane.width(), pane.height(), metrics.cellWidth, metrics.cellHeight,
        1.0);
    QVERIFY(initialGeometry.has_value());
    pane.setWidth(
        (static_cast<qreal>(initialGeometry->columns) + 0.5)
        * metrics.cellWidth);
    QCoreApplication::processEvents();
    QVERIFY(!pane.resizeOverlayVisible());
    // Match Ghostty's initial 250 ms settling window: further layout churn
    // while a pane is first appearing is not a user-visible resize either.
    QTest::qWait(275);
    QVERIFY(!pane.resizeOverlayVisible());

    QSignalSpy textChanged(&pane, &TerminalPane::resizeOverlayTextChanged);
    QSignalSpy visibilityChanged(
        &pane, &TerminalPane::resizeOverlayVisibleChanged);
    pane.setWidth(580.0);
    pane.setWidth(660.0);
    pane.setWidth(740.0);
    QTRY_VERIFY_WITH_TIMEOUT(pane.resizeOverlayVisible(), 1000);
    QCOMPARE(textChanged.count(), 1);
    QCOMPARE(visibilityChanged.count(), 1);

    const auto expected = terminalSessionGeometryForViewport(
        pane.width(), pane.height(), metrics.cellWidth, metrics.cellHeight,
        1.0);
    QVERIFY(expected.has_value());
    QCOMPARE(pane.resizeOverlayText(),
             QStringLiteral("%1 x %2")
                 .arg(expected->columns)
                 .arg(expected->rows));

    QTest::qWait(150);
    pane.setWidth(820.0);
    QTRY_COMPARE_WITH_TIMEOUT(textChanged.count(), 2, 1000);
    QTest::qWait(150);
    QVERIFY(pane.resizeOverlayVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!pane.resizeOverlayVisible(), 300);

    LaunchOptions reloaded = options;
    reloaded.resizeOverlay.mode = ResizeOverlayMode::Always;
    reloaded.resizeOverlay.position = ResizeOverlayPosition::BottomRight;
    reloaded.resizeOverlay.duration = std::chrono::milliseconds(400);
    QSignalSpy rectChanged(&pane, &TerminalPane::resizeOverlayRectChanged);
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(rectChanged.count(), 1);
    QCOMPARE(pane.resizeOverlayRect().topLeft(), QPointF(700.0, 260.0));
    pane.setWidth(900.0);
    QTRY_VERIFY_WITH_TIMEOUT(pane.resizeOverlayVisible(), 1000);

    reloaded.resizeOverlay.mode = ResizeOverlayMode::Never;
    pane.applyRuntimeOptions(reloaded);
    QVERIFY(!pane.resizeOverlayVisible());
    pane.setWidth(1'000.0);
    QCoreApplication::processEvents();
    QVERIFY(!pane.resizeOverlayVisible());

    // A same-turn re-enable must not revive work queued before `never`.
    reloaded.resizeOverlay.mode = ResizeOverlayMode::Always;
    pane.applyRuntimeOptions(reloaded);
    pane.setWidth(1'080.0);
    LaunchOptions disabledAgain = reloaded;
    disabledAgain.resizeOverlay.mode = ResizeOverlayMode::Never;
    pane.applyRuntimeOptions(disabledAgain);
    pane.applyRuntimeOptions(reloaded);
    QCoreApplication::processEvents();
    QVERIFY(!pane.resizeOverlayVisible());
    pane.setWidth(1'160.0);
    QTRY_VERIFY_WITH_TIMEOUT(pane.resizeOverlayVisible(), 1000);
}

void TerminalPaneTest::runsCursorBlinkTimerOnlyWhenNeeded()
{
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(320, 160);
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy sessionEnded(pane, &TerminalPane::sessionEnded);
    auto *cursorTimer = pane->findChild<QTimer *>(
        QString(), Qt::FindDirectChildrenOnly);
    QVERIFY(cursorTimer != nullptr);
    cursorTimer->setTimerType(Qt::PreciseTimer);
    cursorTimer->setInterval(100);
    QSignalSpy timeouts(cursorTimer, &QTimer::timeout);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(sessionEnded.count(), 1, 5000);
    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);

    const auto updateCursor = [controller](bool visible, bool blinking) {
        TerminalUpdate update;
        update.columns = 2;
        update.rows = 1;
        update.fullFrame = true;
        update.foreground = Qt::white;
        update.background = Qt::black;
        update.cursorColor = QColor(QStringLiteral("#ff00ff"));
        update.cursorColorExplicit = true;
        update.cursorVisible = visible;
        update.cursorBlinking = blinking;
        update.cursorStyle = 1;
        TerminalRowUpdate row;
        row.row = 0;
        row.cells.resize(update.columns);
        for (TerminalCell &cell : row.cells) {
            cell.foreground = update.foreground;
            cell.background = update.background;
        }
        update.dirtyRows.append(std::move(row));
        controller->terminalUpdated(update);
    };

    updateCursor(true, false);
    QVERIFY(!cursorTimer->isActive());

    updateCursor(true, true);
    QVERIFY(cursorTimer->isActive());
    const int activeTimerId = cursorTimer->timerId();

    // An ordinary applied frame preserves the running timer and its deadline.
    updateCursor(true, true);
    QCOMPARE(cursorTimer->timerId(), activeTimerId);
    QTRY_COMPARE_WITH_TIMEOUT(timeouts.count(), 1, 1000);

    // PTY activity resets an off-phase block cursor immediately and restarts
    // the full timer interval instead of inheriting the previous deadline.
    cursorTimer->setInterval(10'000);
    const QImage hiddenCursor = window.grabWindow();
    QVERIFY(!hiddenCursor.isNull());
    QVERIFY(approximatelyEqual(hiddenCursor.pixelColor(1, 1), Qt::black));
    const int staleTimerId = cursorTimer->timerId();
    TerminalUpdate reset;
    reset.columns = 2;
    reset.rows = 1;
    reset.resetCursorBlink = true;
    controller->terminalUpdated(reset);
    QVERIFY(cursorTimer->timerId() != staleTimerId);
    const QImage resetCursor = window.grabWindow();
    QVERIFY(!resetCursor.isNull());
    QVERIFY(approximatelyEqual(resetCursor.pixelColor(1, 1),
                               QColor(QStringLiteral("#ff00ff"))));

    timeouts.clear();
    updateCursor(false, true);
    QVERIFY(!cursorTimer->isActive());
    cursorTimer->setInterval(50);
    QTest::qWait(100);
    QCOMPARE(timeouts.count(), 0);

    cursorTimer->setInterval(10'000);
    updateCursor(true, true);
    QVERIFY(cursorTimer->isActive());

    QQuickItem focusTarget(window.contentItem());
    focusTarget.setFocusPolicy(Qt::StrongFocus);
    focusTarget.forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(!pane->hasActiveFocus(), 1000);
    QVERIFY(!cursorTimer->isActive());

    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);
    QVERIFY(cursorTimer->isActive());

    updateCursor(true, false);
    QVERIFY(!cursorTimer->isActive());

    window.close();
    delete pane;
}

void TerminalPaneTest::retainsMainTextRowsAcrossIncrementalUpdates()
{
    qRegisterMetaType<TerminalSearchUpdate>();
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.fontFamily = QFontDatabase::systemFont(
        QFontDatabase::FixedFont).family();
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;
    options.appearance.searchForeground = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#abcdef")));
    options.appearance.searchBackground = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#123456")));
    options.appearance.cursorColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#ff00ff")));
    options.appearance.cursorTextColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#00ffff")));

    QFont testFont(options.fontFamily);
    testFont.setPointSizeF(options.fontSize);
    testFont.setFixedPitch(true);
    testFont.setStyleHint(QFont::Monospace);
    const QFontMetricsF metrics(testFont);
    const qreal cellWidth = std::max(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max(1.0, std::ceil(metrics.height()));

    constexpr int columns = 4;
    constexpr int rows = 4;
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(qCeil(cellWidth * columns), qCeil(cellHeight * (rows + 3)));
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy sessionEnded(pane, &TerminalPane::sessionEnded);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(sessionEnded.count(), 1, 5000);
    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);

    const auto makeRow = [](int row, const QColor &color,
                            bool withGlyph = true) {
        TerminalRowUpdate update;
        update.row = row;
        update.cells.resize(columns);
        for (TerminalCell &cell : update.cells) {
            cell.foreground = color;
            cell.background = Qt::black;
        }
        if (withGlyph) {
            update.cells[0].text = QString(QChar(0x2588));
        }
        return update;
    };
    const std::array<QColor, rows> rowColors{
        QColor(QStringLiteral("#ff0000")),
        QColor(QStringLiteral("#00ff00")),
        QColor(QStringLiteral("#0000ff")),
        QColor(QStringLiteral("#ffff00")),
    };

    quint64 revision = 1;
    TerminalUpdate full;
    full.columns = columns;
    full.rows = rows;
    full.fullFrame = true;
    full.foreground = Qt::white;
    full.background = Qt::black;
    full.cursorColor = QColor(QStringLiteral("#ff00ff"));
    full.cursorColorExplicit = true;
    full.contentRevision = revision;
    for (int row = 0; row < rows; ++row) {
        full.dirtyRows.append(makeRow(row, rowColors[static_cast<size_t>(row)]));
    }
    controller->terminalUpdated(full);
    const QImage initialImage = window.grabWindow();
    QVERIFY(!initialImage.isNull());
    const TerminalPaneRenderProbeSnapshot initial =
        terminalPaneRenderProbe(pane);
    QCOMPARE(initial.rowNodeSerials.size(), rows);
    QCOMPARE(initial.rowBuildCounts.size(), rows);
    for (quint64 count : initial.rowBuildCounts) {
        QVERIFY(count > 0);
    }

    // Metadata-only paints retain every main-text row.
    TerminalUpdate scrollbar;
    scrollbar.columns = columns;
    scrollbar.rows = rows;
    scrollbar.scrollbarChanged = true;
    scrollbar.scrollTotal = 100;
    scrollbar.scrollLength = 20;
    scrollbar.contentRevision = ++revision;
    controller->terminalUpdated(scrollbar);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot metadata =
        terminalPaneRenderProbe(pane);
    QCOMPARE(metadata.rootSerial, initial.rootSerial);
    QCOMPARE(metadata.rowNodeSerials, initial.rowNodeSerials);
    QCOMPARE(metadata.rowBuildCounts, initial.rowBuildCounts);

    // GUI-only overlays rebuild transient layers without touching main text.
    controller->errorOccurred(QStringLiteral("renderer overlay update"));
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot overlay =
        terminalPaneRenderProbe(pane);
    QCOMPARE(overlay.rootSerial, metadata.rootSerial);
    QCOMPARE(overlay.rowNodeSerials, metadata.rowNodeSerials);
    QCOMPARE(overlay.rowBuildCounts, metadata.rowBuildCounts);

    // Two updates coalesced before one render must retain both dirty rows.
    TerminalUpdate firstRow;
    firstRow.columns = columns;
    firstRow.rows = rows;
    firstRow.dirtyRows.append(makeRow(0, QColor(QStringLiteral("#ff8800"))));
    firstRow.contentRevision = ++revision;
    controller->terminalUpdated(firstRow);
    TerminalUpdate thirdRow;
    thirdRow.columns = columns;
    thirdRow.rows = rows;
    thirdRow.dirtyRows.append(makeRow(2, rowColors[2], false));
    thirdRow.contentRevision = ++revision;
    controller->terminalUpdated(thirdRow);

    const QImage sparseImage = window.grabWindow();
    QVERIFY(!sparseImage.isNull());
    const TerminalPaneRenderProbeSnapshot sparse =
        terminalPaneRenderProbe(pane);
    QCOMPARE(sparse.rowNodeSerials, initial.rowNodeSerials);
    const QVector<quint64> sparseBuildCounts{
        initial.rowBuildCounts[0] + 1,
        initial.rowBuildCounts[1],
        initial.rowBuildCounts[2] + 1,
        initial.rowBuildCounts[3],
    };
    QCOMPARE(sparse.rowBuildCounts, sparseBuildCounts);

    const auto centerColor = [&](const QImage &image, int row) {
        const qreal xScale = static_cast<qreal>(image.width()) / window.width();
        const qreal yScale = static_cast<qreal>(image.height()) / window.height();
        return image.pixelColor(
            qBound(0, qRound(0.5 * cellWidth * xScale), image.width() - 1),
            qBound(0, qRound((row + 0.5) * cellHeight * yScale),
                   image.height() - 1));
    };
    QVERIFY(approximatelyEqual(centerColor(sparseImage, 0),
                               QColor(QStringLiteral("#ff8800"))));
    QVERIFY(approximatelyEqual(centerColor(sparseImage, 1), rowColors[1]));
    QVERIFY(approximatelyEqual(centerColor(sparseImage, 2), Qt::black));
    QVERIFY(approximatelyEqual(centerColor(sparseImage, 3), rowColors[3]));

    TerminalUpdate restoreThirdRow;
    restoreThirdRow.columns = columns;
    restoreThirdRow.rows = rows;
    restoreThirdRow.dirtyRows.append(makeRow(2, rowColors[2]));
    restoreThirdRow.contentRevision = ++revision;
    controller->terminalUpdated(restoreThirdRow);
    QVERIFY(!window.grabWindow().isNull());

    TerminalUpdate cursor;
    cursor.columns = columns;
    cursor.rows = rows;
    cursor.cursorChanged = true;
    cursor.cursorVisible = true;
    cursor.cursorBlinking = false;
    cursor.cursorColumn = 0;
    cursor.cursorRow = 0;
    cursor.cursorStyle = 1;
    cursor.cursorColumnSpan = 1;
    cursor.contentRevision = ++revision;
    controller->terminalUpdated(cursor);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot cursorOnFirst =
        terminalPaneRenderProbe(pane);

    cursor.cursorRow = 2;
    cursor.contentRevision = ++revision;
    controller->terminalUpdated(cursor);
    const QImage movedCursorImage = window.grabWindow();
    QVERIFY(!movedCursorImage.isNull());
    const TerminalPaneRenderProbeSnapshot cursorOnThird =
        terminalPaneRenderProbe(pane);
    QCOMPARE(cursorOnThird.rowNodeSerials, cursorOnFirst.rowNodeSerials);
    const QVector<quint64> cursorBuildCounts{
        cursorOnFirst.rowBuildCounts[0] + 1,
        cursorOnFirst.rowBuildCounts[1],
        cursorOnFirst.rowBuildCounts[2] + 1,
        cursorOnFirst.rowBuildCounts[3],
    };
    QCOMPARE(cursorOnThird.rowBuildCounts, cursorBuildCounts);
    QVERIFY(approximatelyEqual(centerColor(movedCursorImage, 0),
                               QColor(QStringLiteral("#ff8800"))));
    QVERIFY(approximatelyEqual(centerColor(movedCursorImage, 2),
                               QColor(QStringLiteral("#00ffff"))));

    TerminalSearchUpdate search;
    search.generation = 1;
    search.contentRevision = revision;
    search.active = true;
    search.complete = true;
    search.totalMatches = 1;
    search.selectedMatch = 0;
    search.columns = columns;
    search.rows = rows;
    search.visibleCellMask = cellMask(columns, rows, {QPoint(0, 1)});
    search.selectedCellMask = QBitArray(columns * rows);
    controller->searchUpdated(search);
    const QImage searchImage = window.grabWindow();
    QVERIFY(!searchImage.isNull());
    const TerminalPaneRenderProbeSnapshot searched =
        terminalPaneRenderProbe(pane);
    QCOMPARE(searched.rootSerial, cursorOnThird.rootSerial);
    for (int row = 0; row < rows; ++row) {
        QCOMPARE(searched.rowBuildCounts[row],
                 cursorOnThird.rowBuildCounts[row] + 1);
    }
    QVERIFY(approximatelyEqual(centerColor(searchImage, 1),
                               QColor(QStringLiteral("#abcdef"))));

    search.active = false;
    controller->searchUpdated(search);
    const QImage clearedSearchImage = window.grabWindow();
    QVERIFY(!clearedSearchImage.isNull());
    const TerminalPaneRenderProbeSnapshot clearedSearch =
        terminalPaneRenderProbe(pane);
    for (int row = 0; row < rows; ++row) {
        QCOMPARE(clearedSearch.rowBuildCounts[row],
                 searched.rowBuildCounts[row] + 1);
    }
    QVERIFY(approximatelyEqual(centerColor(clearedSearchImage, 1),
                               rowColors[1]));

    // Every global text-state dependency invalidates all currently visible
    // rows, including newly visible rows after a shape change.
    TerminalUpdate colors;
    colors.columns = columns;
    colors.rows = rows;
    colors.colorsChanged = true;
    colors.foreground = full.foreground;
    colors.background = full.background;
    colors.cursorColor = full.cursorColor;
    colors.cursorColorExplicit = full.cursorColorExplicit;
    colors.palette = {QColor(QStringLiteral("#112233"))};
    colors.contentRevision = ++revision;
    controller->terminalUpdated(colors);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot paletteChanged =
        terminalPaneRenderProbe(pane);
    QCOMPARE(paletteChanged.rootSerial, clearedSearch.rootSerial);
    QVERIFY(allVisibleRowsRebuilt(clearedSearch, paletteChanged));

    LaunchOptions appearanceChanged = options;
    appearanceChanged.appearance.faintOpacity = 0.75;
    pane->applyRuntimeOptions(appearanceChanged);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot reloadedAppearance =
        terminalPaneRenderProbe(pane);
    QCOMPARE(reloadedAppearance.rootSerial, paletteChanged.rootSerial);
    QVERIFY(allVisibleRowsRebuilt(paletteChanged, reloadedAppearance));

    LaunchOptions fontChanged = appearanceChanged;
    fontChanged.fontSize += 1.0;
    pane->applyRuntimeOptions(fontChanged);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot reloadedFont =
        terminalPaneRenderProbe(pane);
    QCOMPARE(reloadedFont.rootSerial, reloadedAppearance.rootSerial);
    QVERIFY(allVisibleRowsRebuilt(reloadedAppearance, reloadedFont));

    pane->setWidth(std::max(1.0, pane->width() - cellWidth));
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot resized =
        terminalPaneRenderProbe(pane);
    QCOMPARE(resized.rootSerial, reloadedFont.rootSerial);
    QVERIFY(allVisibleRowsRebuilt(reloadedFont, resized));

    window.close();
    delete pane;
}

void TerminalPaneTest::retainsTextWhileDimmingUnfocusedSplits()
{
    qRegisterMetaType<TerminalUpdate>();

    const QColor background(QStringLiteral("#204060"));
    const QColor firstFill(QStringLiteral("#d08020"));
    const QColor secondFill(QStringLiteral("#20c080"));
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = background;
    options.splitAppearance = {
        .unfocusedOpacity = 0.7,
        .unfocusedFill = firstFill,
    };

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(360, 180);
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *focusSink = new QQuickItem(window.contentItem());
    focusSink->setSize(QSizeF(1.0, 1.0));
    focusSink->setPosition(QPointF(window.width() - 1.0,
                                   window.height() - 1.0));
    focusSink->setFocusPolicy(Qt::StrongFocus);
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy sessionEnded(pane, &TerminalPane::sessionEnded);
    QSignalSpy runtimeOptions(
        controller, &TerminalController::runtimeOptionsRequested);

    window.show();
    window.requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(sessionEnded.count(), 1, 5000);

    TerminalUpdate frame;
    frame.columns = 5;
    frame.rows = 3;
    frame.fullFrame = true;
    frame.foreground = Qt::white;
    frame.background = background;
    frame.cursorVisible = false;
    frame.contentRevision = 1;
    for (int row = 0; row < frame.rows; ++row) {
        TerminalRowUpdate rowUpdate;
        rowUpdate.row = row;
        rowUpdate.cells.resize(frame.columns);
        for (TerminalCell &cell : rowUpdate.cells) {
            cell.foreground = Qt::white;
            cell.background = background;
        }
        rowUpdate.cells[0].text = QStringLiteral("x");
        frame.dirtyRows.append(std::move(rowUpdate));
    }
    controller->terminalUpdated(frame);
    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);

    const QImage baselineImage = window.grabWindow();
    QVERIFY(!baselineImage.isNull());
    const TerminalPaneRenderProbeSnapshot baseline =
        terminalPaneRenderProbe(pane);
    QVERIFY(baseline.rootSerial != 0);
    QVERIFY(baseline.unfocusedSplitOverlaySerial != 0);
    QVERIFY(baseline.unfocusedSplitOverlayRect.isEmpty());
    QVERIFY(!baseline.rowNodeSerials.isEmpty());

    const QPointF quietPoint(pane->width() - 20.0, 20.0);
    const QPointF statusPoint(pane->width() - 20.0,
                              pane->height() - 10.0);
    const QColor quietBaseline = itemPixel(
        window, *pane, baselineImage, quietPoint);
    const QColor statusBaseline = itemPixel(
        window, *pane, baselineImage, statusPoint);

    const auto verifyTextRetained = [](const auto &before,
                                       const auto &after) {
        QCOMPARE(after.rootSerial, before.rootSerial);
        QCOMPARE(after.unfocusedSplitOverlaySerial,
                 before.unfocusedSplitOverlaySerial);
        QCOMPARE(after.rowNodeSerials, before.rowNodeSerials);
        QCOMPARE(after.rowBuildCounts, before.rowBuildCounts);
        QVERIFY(after.paintSerial > before.paintSerial);
    };

    pane->setSplit(true);
    const QImage dimmedImage = window.grabWindow();
    QVERIFY(!dimmedImage.isNull());
    const TerminalPaneRenderProbeSnapshot dimmed =
        terminalPaneRenderProbe(pane);
    verifyTextRetained(baseline, dimmed);
    QCOMPARE(dimmed.unfocusedSplitOverlayRect, pane->boundingRect());
    QColor expectedNodeColor = firstFill;
    expectedNodeColor.setAlphaF(0.3);
    QCOMPARE(dimmed.unfocusedSplitOverlayColor, expectedNodeColor);
    QVERIFY(approximatelyEqual(
        itemPixel(window, *pane, dimmedImage, quietPoint),
        sourceOver(quietBaseline, firstFill, 0.3)));
    QVERIFY(approximatelyEqual(
        itemPixel(window, *pane, dimmedImage, statusPoint),
        sourceOver(statusBaseline, firstFill, 0.3)));

    LaunchOptions reloaded = options;
    // GTK serializes the complementary alpha to two decimals: 1 - 0.676
    // becomes 0.32, rather than the unquantized 0.324.
    reloaded.splitAppearance.unfocusedOpacity = 0.676;
    reloaded.splitAppearance.unfocusedFill = secondFill;
    pane->applyRuntimeOptions(reloaded);
    QCOMPARE(runtimeOptions.count(), 0);
    const QImage reloadedImage = window.grabWindow();
    QVERIFY(!reloadedImage.isNull());
    const TerminalPaneRenderProbeSnapshot reloadedProbe =
        terminalPaneRenderProbe(pane);
    verifyTextRetained(dimmed, reloadedProbe);
    expectedNodeColor = secondFill;
    expectedNodeColor.setAlphaF(0.32);
    QCOMPARE(reloadedProbe.unfocusedSplitOverlayColor, expectedNodeColor);
    QVERIFY(approximatelyEqual(
        itemPixel(window, *pane, reloadedImage, quietPoint),
        sourceOver(quietBaseline, secondFill, 0.32)));

    const QColor configuredBackground(QStringLiteral("#102030"));
    reloaded.appearance.backgroundColor = configuredBackground;
    pane->applyRuntimeOptions(reloaded);
    QCOMPARE(runtimeOptions.count(), 1);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot explicitBackgroundReload =
        terminalPaneRenderProbe(pane);
    QCOMPARE(explicitBackgroundReload.rootSerial,
             reloadedProbe.rootSerial);
    QCOMPARE(explicitBackgroundReload.unfocusedSplitOverlaySerial,
             reloadedProbe.unfocusedSplitOverlaySerial);
    QCOMPARE(explicitBackgroundReload.unfocusedSplitOverlayColor,
             expectedNodeColor);

    reloaded.splitAppearance.unfocusedFill.reset();
    pane->applyRuntimeOptions(reloaded);
    const QImage fallbackImage = window.grabWindow();
    QVERIFY(!fallbackImage.isNull());
    const TerminalPaneRenderProbeSnapshot fallback =
        terminalPaneRenderProbe(pane);
    verifyTextRetained(explicitBackgroundReload, fallback);
    expectedNodeColor = configuredBackground;
    expectedNodeColor.setAlphaF(0.32);
    QCOMPARE(fallback.unfocusedSplitOverlayColor, expectedNodeColor);
    QVERIFY(approximatelyEqual(
        itemPixel(window, *pane, fallbackImage, quietPoint),
        sourceOver(quietBaseline, configuredBackground, 0.32)));

    const QColor latestBackground(QStringLiteral("#301020"));
    reloaded.appearance.backgroundColor = latestBackground;
    pane->applyRuntimeOptions(reloaded);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot latestFallback =
        terminalPaneRenderProbe(pane);
    QCOMPARE(latestFallback.rootSerial, fallback.rootSerial);
    QCOMPARE(latestFallback.unfocusedSplitOverlaySerial,
             fallback.unfocusedSplitOverlaySerial);
    expectedNodeColor = latestBackground;
    expectedNodeColor.setAlphaF(0.32);
    QCOMPARE(latestFallback.unfocusedSplitOverlayColor,
             expectedNodeColor);

    reloaded.splitAppearance.unfocusedFill = secondFill;
    pane->applyRuntimeOptions(reloaded);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot explicitAgain =
        terminalPaneRenderProbe(pane);
    verifyTextRetained(latestFallback, explicitAgain);
    expectedNodeColor = secondFill;
    expectedNodeColor.setAlphaF(0.32);
    QCOMPARE(explicitAgain.unfocusedSplitOverlayColor,
             expectedNodeColor);

    reloaded.splitAppearance.unfocusedOpacity = 1.0;
    pane->applyRuntimeOptions(reloaded);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot disabled =
        terminalPaneRenderProbe(pane);
    verifyTextRetained(explicitAgain, disabled);
    QVERIFY(disabled.unfocusedSplitOverlayRect.isEmpty());

    reloaded.splitAppearance.unfocusedOpacity =
        std::numeric_limits<double>::quiet_NaN();
    pane->applyRuntimeOptions(reloaded);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot defensiveDefault =
        terminalPaneRenderProbe(pane);
    verifyTextRetained(disabled, defensiveDefault);
    expectedNodeColor = secondFill;
    expectedNodeColor.setAlphaF(0.3);
    QCOMPARE(defensiveDefault.unfocusedSplitOverlayColor,
             expectedNodeColor);

    QVERIFY(pane->executeConfiguredAction(QStringLiteral("start_search")));
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot searching =
        terminalPaneRenderProbe(pane);
    verifyTextRetained(defensiveDefault, searching);
    QVERIFY(searching.unfocusedSplitOverlayRect.isEmpty());

    pane->endSearchUi();
    QVERIFY(!pane->searchUiActive());
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot searchEnded =
        terminalPaneRenderProbe(pane);
    verifyTextRetained(searching, searchEnded);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), pane, 1000);
    QVERIFY(searchEnded.unfocusedSplitOverlayRect.isEmpty());

    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot externallyFocused =
        terminalPaneRenderProbe(pane);
    verifyTextRetained(searchEnded, externallyFocused);
    QCOMPARE(externallyFocused.unfocusedSplitOverlayRect,
             pane->boundingRect());

    pane->setSplit(false);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot unsplit =
        terminalPaneRenderProbe(pane);
    verifyTextRetained(externallyFocused, unsplit);
    QVERIFY(unsplit.unfocusedSplitOverlayRect.isEmpty());

    window.close();
    delete pane;
}

void TerminalPaneTest::rebuildsMainTextRowsAfterWindowChange()
{
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.fontFamily = QFontDatabase::systemFont(
        QFontDatabase::FixedFont).family();
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;

    QFont testFont(options.fontFamily);
    testFont.setPointSizeF(options.fontSize);
    testFont.setFixedPitch(true);
    testFont.setStyleHint(QFont::Monospace);
    const QFontMetricsF metrics(testFont);
    const qreal cellWidth = std::max(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max(1.0, std::ceil(metrics.height()));

    constexpr int columns = 2;
    constexpr int rows = 3;
    const QSize windowSize(qCeil(cellWidth * columns),
                           qCeil(cellHeight * (rows + 3)));
    QQuickWindow firstWindow;
    QQuickWindow secondWindow;
    firstWindow.setColor(Qt::black);
    secondWindow.setColor(Qt::black);
    firstWindow.resize(windowSize);
    secondWindow.resize(windowSize);

    auto *pane = new TerminalPane(options);
    pane->setParentItem(firstWindow.contentItem());
    pane->setSize(windowSize);
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy sessionEnded(pane, &TerminalPane::sessionEnded);
    QSignalSpy resizeRequested(controller,
                               &TerminalController::resizeRequested);

    firstWindow.show();
    secondWindow.show();
    QTRY_VERIFY_WITH_TIMEOUT(firstWindow.isExposed(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(secondWindow.isExposed(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(sessionEnded.count(), 1, 5000);

    TerminalUpdate full;
    full.columns = columns;
    full.rows = rows;
    full.fullFrame = true;
    full.foreground = Qt::white;
    full.background = Qt::black;
    full.contentRevision = 1;
    for (int row = 0; row < rows; ++row) {
        TerminalRowUpdate rowUpdate;
        rowUpdate.row = row;
        rowUpdate.cells.resize(columns);
        for (TerminalCell &cell : rowUpdate.cells) {
            cell.foreground = Qt::white;
            cell.background = Qt::black;
        }
        rowUpdate.cells[0].text = QString(QChar(0x2588));
        full.dirtyRows.append(std::move(rowUpdate));
    }
    controller->terminalUpdated(full);
    QVERIFY(!firstWindow.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot firstRoot =
        terminalPaneRenderProbe(pane);
    QCOMPARE(firstRoot.rowBuildCounts.size(), rows);
    for (quint64 count : firstRoot.rowBuildCounts) {
        QVERIFY(count > 0);
    }

    pane->setParentItem(nullptr);
    firstWindow.update();
    QVERIFY(!firstWindow.grabWindow().isNull());
    resizeRequested.clear();
    pane->setParentItem(secondWindow.contentItem());
    pane->setSize(windowSize);
    QTRY_VERIFY_WITH_TIMEOUT(!resizeRequested.isEmpty(), 1000);
    resizeRequested.clear();
    Q_EMIT secondWindow.screenChanged(secondWindow.screen());
    QCOMPARE(resizeRequested.count(), 1);
    resizeRequested.clear();
    QEvent devicePixelRatioChange(QEvent::DevicePixelRatioChange);
    QCoreApplication::sendEvent(&secondWindow, &devicePixelRatioChange);
    QCOMPARE(resizeRequested.count(), 1);
    pane->update();
    const QImage rebuiltImage = secondWindow.grabWindow();
    QVERIFY(!rebuiltImage.isNull());
    const TerminalPaneRenderProbeSnapshot secondRoot =
        terminalPaneRenderProbe(pane);
    QVERIFY(secondRoot.rootSerial != firstRoot.rootSerial);
    QCOMPARE(secondRoot.rowBuildCounts.size(), rows);
    for (int row = 0; row < rows; ++row) {
        QVERIFY(secondRoot.rowBuildCounts[row] > 0);
        QVERIFY(secondRoot.rowNodeSerials[row]
                != firstRoot.rowNodeSerials[row]);
        const qreal xScale = static_cast<qreal>(rebuiltImage.width())
            / secondWindow.width();
        const qreal yScale = static_cast<qreal>(rebuiltImage.height())
            / secondWindow.height();
        const QColor pixel = rebuiltImage.pixelColor(
            qRound(0.5 * cellWidth * xScale),
            qRound((row + 0.5) * cellHeight * yScale));
        QVERIFY(approximatelyEqual(pixel, Qt::white));
    }

    TerminalUpdate partial;
    partial.columns = columns;
    partial.rows = rows;
    partial.contentRevision = 2;
    TerminalRowUpdate changedRow;
    changedRow.row = 1;
    changedRow.cells.resize(columns);
    for (TerminalCell &cell : changedRow.cells) {
        cell.foreground = Qt::red;
        cell.background = Qt::black;
    }
    changedRow.cells[0].text = QString(QChar(0x2588));
    partial.dirtyRows.append(std::move(changedRow));
    controller->terminalUpdated(partial);
    QVERIFY(!secondWindow.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot sparse =
        terminalPaneRenderProbe(pane);
    QCOMPARE(sparse.rootSerial, secondRoot.rootSerial);
    QCOMPARE(sparse.rowNodeSerials, secondRoot.rowNodeSerials);
    QVector<quint64> sparseBuildCounts = secondRoot.rowBuildCounts;
    ++sparseBuildCounts[1];
    QCOMPARE(sparse.rowBuildCounts, sparseBuildCounts);

    pane->setParentItem(nullptr);
    delete pane;
    firstWindow.close();
    secondWindow.close();
}

void TerminalPaneTest::routesSearchActionsAndRetainsUiState()
{
    qRegisterMetaType<TerminalSearchUpdate>();
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("sleep 5"),
    };
    options.hold = true;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy activeSpy(&pane, &TerminalPane::searchUiActiveChanged);
    QSignalSpy textSpy(&pane, &TerminalPane::searchUiTextChanged);
    QSignalSpy labelSpy(&pane, &TerminalPane::searchMatchLabelChanged);
    QSignalSpy focusSpy(&pane, &TerminalPane::searchUiFocusRequested);

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("start_search")));
    QVERIFY(pane.searchUiActive());
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(focusSpy.count(), 1);

    pane.setSearchUiText(QStringLiteral("visible entry"));
    QCOMPARE(pane.searchUiText(), QStringLiteral("visible entry"));
    QCOMPARE(textSpy.count(), 1);
    QVERIFY(controller->searchExpected());

    // Engine-only actions do not open, close, or rewrite the retained entry.
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("search:backend")));
    QCOMPARE(pane.searchUiText(), QStringLiteral("visible entry"));
    QVERIFY(pane.searchUiActive());
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("navigate_search:next")));

    TerminalSearchUpdate progress;
    progress.generation = 99;
    progress.contentRevision = 1;
    progress.active = true;
    progress.totalMatches = 4;
    progress.selectedMatch = -1;
    progress.columns = 80;
    progress.rows = 24;
    controller->searchUpdated(progress);
    QCOMPARE(pane.searchMatchLabel(), QStringLiteral("0/4"));
    progress.selectedMatch = 1;
    controller->searchUpdated(progress);
    QCOMPARE(pane.searchMatchLabel(), QStringLiteral("2/4"));
    QVERIFY(labelSpy.count() >= 2);

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("end_search")));
    QVERIFY(!pane.searchUiActive());
    QCOMPARE(pane.searchUiText(), QStringLiteral("visible entry"));
    QCOMPARE(pane.searchMatchLabel(), QStringLiteral("0/0"));
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("end_search")));

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("search:detached")));
    QVERIFY(!pane.searchUiActive());
    QCOMPARE(pane.searchUiText(), QStringLiteral("visible entry"));
    QVERIFY(controller->searchExpected());
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("start_search")));
    QVERIFY(pane.searchUiActive());
    QCOMPARE(pane.searchUiText(), QStringLiteral("visible entry"));
    const int focusBeforeActivation = focusSpy.count();
    pane.focusTerminal();
    QCOMPARE(focusSpy.count(), focusBeforeActivation + 1);

    pane.endSearchUi();
    QVERIFY(!pane.searchUiActive());
    QKeyEvent openSearch(
        QEvent::KeyPress, Qt::Key_F,
        Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("f"));
    QCoreApplication::sendEvent(&pane, &openSearch);
    QVERIFY(openSearch.isAccepted());
    QVERIFY(pane.searchUiActive());
    QKeyEvent closeSearch(
        QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &closeSearch);
    QVERIFY(closeSearch.isAccepted());
    QVERIFY(!pane.searchUiActive());

    // Empty search actions mirror Ghostty's performability while still
    // dispatching end_search so a stale frontend overlay is cleaned up.
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("search:")));
    pane.setSearchUiText(QString{});
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("start_search")));
    QVERIFY(pane.searchUiActive());
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("end_search")));
    QVERIFY(!pane.searchUiActive());

    // No selection means Ghostty's search_selection action is not
    // performable and must not synthesize an empty query.
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("search_selection")));
}

void TerminalPaneTest::replacesStartingFrameInsteadOfAccumulatingSceneRoots()
{
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "sleep 0.2; "
            "printf '\\033[2J\\033[H\\033[10;10Hscene-root-marker'; "
            "sleep 3"),
    };
    options.hold = true;

    QQuickWindow window;
    const QColor background(QStringLiteral("#1e222a"));
    window.setColor(background);
    window.resize(640, 384);

    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updateSpy(controller, &TerminalController::terminalUpdated);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updateSpy, QStringLiteral("scene-root-marker")), 5000);

    // Allow multiple scene-graph updates, including a cursor blink, before
    // grabbing the rendered pane. A retained initial root would still draw
    // "Starting terminal…" in this otherwise cleared region.
    QTest::qWait(700);
    const QImage image = window.grabWindow();
    QVERIFY(!image.isNull());

    const qreal scale = static_cast<qreal>(image.width()) / window.width();
    const QRect startingTextRegion(
        qRound(8.0 * scale), qRound(8.0 * scale),
        qRound(220.0 * scale), qRound(40.0 * scale));
    int unexpectedPixels = 0;
    for (int y = startingTextRegion.top(); y < startingTextRegion.bottom(); ++y) {
        for (int x = startingTextRegion.left(); x < startingTextRegion.right(); ++x) {
            if (!approximatelyEqual(image.pixelColor(x, y), background)) {
                ++unexpectedPixels;
            }
        }
    }
    QCOMPARE(unexpectedPixels, 0);

    int renderedPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (!approximatelyEqual(image.pixelColor(x, y), background)) {
                ++renderedPixels;
            }
        }
    }
    QVERIFY(renderedPixels > 20);

    window.close();
    delete pane;
}

void TerminalPaneTest::reloadsFontWithoutOverwritingManualZoom()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.fontSize = 12.0;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy runtimeOptions(
        controller, &TerminalController::runtimeOptionsRequested);
    LaunchOptions reloaded = options;
    reloaded.fontSize = 14.0;
    reloaded.fontFamily = QStringLiteral("Monospace");
    reloaded.appearance.foregroundColor = QColor(QStringLiteral("#123456"));
    reloaded.scrollbackLimit = {
        .value = 321,
        .unit = ScrollbackLimitUnit::Bytes,
    };
    reloaded.selectionClipboard = {
        .trimTrailingSpaces = false,
        .copyOnSelect = TerminalCopyOnSelectMode::PrimaryAndClipboard,
        .clearOnTyping = false,
        .clearOnCopy = true,
    };
    reloaded.clipboardPaste = {
        .protection = false,
        .bracketedSafe = true,
    };
    reloaded.middleClickAction = MiddleClickAction::Ignore;
    reloaded.linkUrl = false;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.fontPointSize(), 14.0);
    const LaunchOptions splitOptions = pane.splitLaunchOptions(reloaded);
    QCOMPARE(splitOptions.fontFamily, QStringLiteral("Monospace"));
    QCOMPARE(splitOptions.fontSize, 14.0);
    QCOMPARE(splitOptions.workingDirectory, QDir::tempPath());
    QVERIFY(splitOptions.program.isEmpty());
    QVERIFY(!splitOptions.hold);
    QCOMPARE(splitOptions.scrollbackLimit, reloaded.scrollbackLimit);
    QCOMPARE(splitOptions.appearance, reloaded.appearance);
    QCOMPARE(splitOptions.selectionClipboard, reloaded.selectionClipboard);
    QCOMPARE(splitOptions.clipboardPaste, reloaded.clipboardPaste);
    QCOMPARE(splitOptions.middleClickAction, reloaded.middleClickAction);
    QCOMPARE(splitOptions.linkUrl, reloaded.linkUrl);
    QCOMPARE(splitOptions.splitInheritWorkingDirectory,
             reloaded.splitInheritWorkingDirectory);
    QCOMPARE(runtimeOptions.count(), 1);
    QCOMPARE(qvariant_cast<TerminalSessionRuntimeOptions>(
                 runtimeOptions.constFirst().constFirst()),
             toTerminalSessionRuntimeOptions(reloaded));

    LaunchOptions splitBase = reloaded;
    splitBase.workingDirectory = QDir::currentPath();
    splitBase.inheritWorkingDirectory = true;
    splitBase.splitInheritWorkingDirectory = false;
    const LaunchOptions fallback = pane.splitLaunchOptions(splitBase);
    QCOMPARE(fallback.workingDirectory, QDir::currentPath());
    QVERIFY(fallback.inheritWorkingDirectory);
    QCOMPARE(fallback.fontFamily, QStringLiteral("Monospace"));
    QCOMPARE(fallback.fontSize, 14.0);

    splitBase.splitInheritWorkingDirectory = true;
    const LaunchOptions directoryInherited =
        pane.splitLaunchOptions(splitBase);
    QCOMPARE(directoryInherited.workingDirectory, QDir::tempPath());
    QVERIFY(!directoryInherited.inheritWorkingDirectory);

    pane.zoomIn();
    QCOMPARE(pane.fontPointSize(), 15.0);
    reloaded.fontSize = 10.0;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.fontPointSize(), 15.0);
    const LaunchOptions inherited = pane.splitLaunchOptions(reloaded);
    QCOMPARE(inherited.fontSize, 15.0);

    LaunchOptions tabBase = reloaded;
    tabBase.workingDirectory = QDir::currentPath();
    tabBase.inheritWorkingDirectory = true;
    tabBase.fontFamily = QStringLiteral("Configured Family");
    tabBase.tabInheritWorkingDirectory = false;
    tabBase.windowInheritFontSize = false;
    const LaunchOptions tabFallback = pane.tabLaunchOptions(tabBase);
    QCOMPARE(tabFallback.workingDirectory, QDir::currentPath());
    QVERIFY(tabFallback.inheritWorkingDirectory);
    QCOMPARE(tabFallback.fontFamily, QStringLiteral("Configured Family"));
    QCOMPARE(tabFallback.fontSize, 10.0);
    QVERIFY(tabFallback.program.isEmpty());
    QVERIFY(!tabFallback.hold);

    tabBase.tabInheritWorkingDirectory = true;
    tabBase.windowInheritFontSize = true;
    const LaunchOptions tabInherited = pane.tabLaunchOptions(tabBase);
    QCOMPARE(tabInherited.workingDirectory, QDir::tempPath());
    QVERIFY(!tabInherited.inheritWorkingDirectory);
    QCOMPARE(tabInherited.fontFamily, QStringLiteral("Configured Family"));
    QCOMPARE(tabInherited.fontSize, 15.0);

    // A split inherits the effective size as its initial config/reset target,
    // but starts unadjusted so the next live reload replaces that target.
    LaunchOptions childOptions = inherited;
    childOptions.program = {QStringLiteral("/bin/true")};
    childOptions.hold = true;
    TerminalPane child(childOptions);
    QCOMPARE(child.fontPointSize(), 15.0);
    child.resetZoom();
    QCOMPARE(child.fontPointSize(), 15.0);

    LaunchOptions nextReload = reloaded;
    nextReload.fontSize = 9.0;
    pane.applyRuntimeOptions(nextReload);
    child.applyRuntimeOptions(nextReload);
    QCOMPARE(pane.fontPointSize(), 15.0);
    QCOMPARE(child.fontPointSize(), 9.0);

    pane.resetZoom();
    QCOMPARE(pane.fontPointSize(), 9.0);
    nextReload.fontSize = 11.0;
    pane.applyRuntimeOptions(nextReload);
    child.applyRuntimeOptions(nextReload);
    QCOMPARE(pane.fontPointSize(), 11.0);
    QCOMPARE(child.fontPointSize(), 11.0);
}

void TerminalPaneTest::executesTypedFontSizeActions()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.fontSize = 12.0;

    TerminalPane pane(options);
    pane.setSize(QSizeF(20.0, 20.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy changed(&pane, &TerminalPane::fontPointSizeChanged);
    QSignalSpy resized(controller, &TerminalController::resizeRequested);

    // A valid negative delta is a visual no-op, but still establishes the
    // manual-adjustment lifecycle and therefore blocks the next live reload.
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("increase_font_size:-2")));
    QCOMPARE(pane.fontPointSize(), 12.0);
    QCOMPARE(changed.count(), 0);
    LaunchOptions reloaded = options;
    reloaded.fontSize = 10.0;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.fontPointSize(), 12.0);

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("reset_font_size")));
    QCOMPARE(pane.fontPointSize(), 10.0);
    reloaded.fontSize = 11.0;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.fontPointSize(), 11.0);

    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("increase_font_size:1.5")));
    QCOMPARE(pane.fontPointSize(), 12.5);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("decrease_font_size:.5")));
    QCOMPARE(pane.fontPointSize(), 12.0);

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("set_font_size:300")));
    QCOMPARE(pane.fontPointSize(), 255.0);
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("set_font_size:-3")));
    QCOMPARE(pane.fontPointSize(), 1.0);

    resized.clear();
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("increase_font_size:nan")));
    QCOMPARE(pane.fontPointSize(), 255.0);
    QVERIFY(!resized.isEmpty());
    QCOMPARE(resized.constLast().at(0).toInt(), 1);
    QCOMPARE(resized.constLast().at(1).toInt(), 1);

    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("decrease_font_size:infinity")));
    QCOMPARE(pane.fontPointSize(), 1.0);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("increase_font_size:-inf")));
    QCOMPARE(pane.fontPointSize(), 1.0);
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("set_font_size:nan")));
    QCOMPARE(pane.fontPointSize(), 255.0);
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("set_font_size:-inf")));
    QCOMPARE(pane.fontPointSize(), 1.0);

    LaunchOptions oversized = options;
    oversized.fontSize = 300.0;
    TerminalPane oversizedPane(oversized);
    QVERIFY(oversizedPane.executeConfiguredAction(
        QStringLiteral("decrease_font_size:300")));
    QCOMPARE(oversizedPane.fontPointSize(), 45.0);
    oversizedPane.resetZoom();
    QCOMPARE(oversizedPane.fontPointSize(), 300.0);
    QVERIFY(oversizedPane.executeConfiguredAction(
        QStringLiteral("decrease_font_size:1")));
    // Ghostty applies an asymmetric lower clamp here; it does not globally
    // clamp an existing configured size down to the action maximum.
    QCOMPARE(oversizedPane.fontPointSize(), 299.0);

    // Reload clamps the displayed size but retains the raw configured value
    // as reset_font_size's target, matching Surface.updateConfig.
    TerminalPane reloadBounds(options);
    LaunchOptions oversizedReload = options;
    oversizedReload.fontSize = 300.0;
    reloadBounds.applyRuntimeOptions(oversizedReload);
    QCOMPARE(reloadBounds.fontPointSize(), 255.0);
    reloadBounds.resetZoom();
    QCOMPARE(reloadBounds.fontPointSize(), 300.0);
}

void TerminalPaneTest::packagesInputMethodLifecycleAsOneWorkerRequest()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 5"),
    };
    options.hold = true;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy requests(controller, &TerminalController::inputMethodRequested);

    QInputMethodEvent commitOnly;
    commitOnly.setCommitString(QStringLiteral("é"));
    QCoreApplication::sendEvent(&pane, &commitOnly);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(qvariant_cast<TerminalInputMethodInput>(
                 requests.takeFirst().constFirst()),
             TerminalInputMethodInput{.commitText = QStringLiteral("é")});

    QInputMethodEvent start(QStringLiteral("compose"), {});
    QCoreApplication::sendEvent(&pane, &start);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(qvariant_cast<TerminalInputMethodInput>(
                 requests.takeFirst().constFirst()),
             TerminalInputMethodInput{.preeditTransition = true});

    QInputMethodEvent end(QString{}, {});
    QCoreApplication::sendEvent(&pane, &end);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(qvariant_cast<TerminalInputMethodInput>(
                 requests.takeFirst().constFirst()),
             TerminalInputMethodInput{.preeditTransition = true});

    // A present-but-empty callback is a transition even with no old preedit.
    const QString presentEmpty = QString::fromLatin1("", 0);
    QVERIFY(presentEmpty.isEmpty());
    QVERIFY(!presentEmpty.isNull());
    QInputMethodEvent emptyUpdate(presentEmpty, {});
    QCoreApplication::sendEvent(&pane, &emptyUpdate);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(qvariant_cast<TerminalInputMethodInput>(
                 requests.takeFirst().constFirst()),
             TerminalInputMethodInput{.preeditTransition = true});

    QInputMethodEvent noOp;
    QCoreApplication::sendEvent(&pane, &noOp);
    QCOMPARE(requests.count(), 0);

    QInputMethodEvent combined(QStringLiteral("next"), {});
    combined.setCommitString(QStringLiteral("x"));
    QCoreApplication::sendEvent(&pane, &combined);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(qvariant_cast<TerminalInputMethodInput>(
                 requests.takeFirst().constFirst()),
             (TerminalInputMethodInput{
                 .commitText = QStringLiteral("x"),
                 .preeditTransition = true,
             }));
}

void TerminalPaneTest::writesClipboardDestinations()
{
    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const bool supportsPrimary = clipboard->supportsSelection();
    const QString standardSentinel =
        QStringLiteral("clipboard-standard-sentinel");
    const QString primarySentinel =
        QStringLiteral("clipboard-primary-sentinel");
    const auto resetClipboards = [&] {
        clipboard->setText(standardSentinel, QClipboard::Clipboard);
        if (supportsPrimary) {
            clipboard->setText(primarySentinel, QClipboard::Selection);
        }
    };

    resetClipboards();
    writeTerminalClipboard(
        clipboard, QStringLiteral("standard-write"),
        TerminalClipboardDestination::Standard);
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard),
                 QStringLiteral("standard-write"));
    if (supportsPrimary) {
        QCOMPARE(clipboard->text(QClipboard::Selection), primarySentinel);
    }

    resetClipboards();
    writeTerminalClipboard(
        clipboard, QStringLiteral("primary-write"),
        TerminalClipboardDestination::Primary);
    if (supportsPrimary) {
        QTRY_COMPARE(clipboard->text(QClipboard::Selection),
                     QStringLiteral("primary-write"));
        QCOMPARE(clipboard->text(QClipboard::Clipboard), standardSentinel);
    } else {
        QTRY_COMPARE(clipboard->text(QClipboard::Clipboard),
                     QStringLiteral("primary-write"));
    }

    resetClipboards();
    writeTerminalClipboard(
        clipboard, QStringLiteral("both-write"),
        TerminalClipboardDestination::PrimaryAndStandard);
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard),
                 QStringLiteral("both-write"));
    if (supportsPrimary) {
        QTRY_COMPARE(clipboard->text(QClipboard::Selection),
                     QStringLiteral("both-write"));
    }

    clipboard->clear(QClipboard::Clipboard);
    if (supportsPrimary) {
        clipboard->clear(QClipboard::Selection);
    }
}

void TerminalPaneTest::copiesRawEffectiveSurfaceTitle()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::PrimaryAndClipboard;

    TerminalPane pane(options);
    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const bool supportsPrimary = clipboard->supportsSelection();
    const QString standardSentinel = QStringLiteral("standard sentinel");
    const QString primarySentinel = QStringLiteral("primary sentinel");
    const QString copyAction = QStringLiteral("copy_title_to_clipboard");
    const auto resetClipboards = [&] {
        clipboard->setText(standardSentinel, QClipboard::Clipboard);
        if (supportsPrimary) {
            clipboard->setText(primarySentinel, QClipboard::Selection);
        }
    };
    const auto verifyPrimaryUnchanged = [&] {
        if (supportsPrimary) {
            QCOMPARE(clipboard->text(QClipboard::Selection), primarySentinel);
        }
    };

    // The visible argv[0] fallback is presentation only and is not copyable.
    resetClipboards();
    QCOMPARE(pane.title(), QStringLiteral("true"));
    QVERIFY(!pane.effectiveSurfaceTitle().has_value());
    QVERIFY(!pane.executeConfiguredAction(copyAction));
    QCOMPARE(clipboard->text(QClipboard::Clipboard), standardSentinel);
    verifyPrimaryUnchanged();

    // An explicit empty base remains a title-layer value, but upstream treats
    // it as a copy no-op just like absence.
    pane.setSurfaceTitle(QString{});
    QVERIFY(pane.effectiveSurfaceTitle().has_value());
    QVERIFY(pane.effectiveSurfaceTitle()->isEmpty());
    QVERIFY(!pane.executeConfiguredAction(copyAction));
    QCOMPARE(clipboard->text(QClipboard::Clipboard), standardSentinel);
    verifyPrimaryUnchanged();

    const QString baseTitle = QStringLiteral("  base 👻  ");
    pane.setSurfaceTitle(baseTitle);
    QVERIFY(pane.executeConfiguredAction(copyAction));
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard), baseTitle);
    verifyPrimaryUnchanged();

    const QString overrideTitle = QStringLiteral("  override 🌐  ");
    pane.setSurfaceTitleOverride(
        std::optional<QString>{overrideTitle});
    pane.setSurfaceTitle(QStringLiteral("new hidden base"));
    QVERIFY(pane.executeConfiguredAction(copyAction));
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard), overrideTitle);
    verifyPrimaryUnchanged();

    pane.setSurfaceTitleOverride(std::nullopt);
    QVERIFY(pane.executeConfiguredAction(copyAction));
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard),
                 QStringLiteral("new hidden base"));
    verifyPrimaryUnchanged();

    clipboard->clear(QClipboard::Clipboard);
    if (supportsPrimary) {
        clipboard->clear(QClipboard::Selection);
    }
}

void TerminalPaneTest::reloadsMiddleClickClipboardPolicy()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty -echo; printf '\\033[?1000h'; exec cat >/dev/null"),
    };
    options.hold = true;
    options.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    options.middleClickAction = MiddleClickAction::PrimaryPaste;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);
    QSignalSpy mouse(controller, &TerminalController::mouseRequested);
    QSignalSpy unsafe(&pane, &TerminalPane::unsafePasteRequested);
    QTRY_VERIFY_WITH_TIMEOUT(controller->mouseTracking(), 5000);

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const bool supportsPrimary = clipboard->supportsSelection();
    const QString standardText = QStringLiteral("middle-standard");
    const QString primaryText = QStringLiteral("middle-primary");
    clipboard->setText(standardText, QClipboard::Clipboard);
    if (supportsPrimary) {
        clipboard->setText(primaryText, QClipboard::Selection);
    }

    const auto pressMiddleButton = [&pane](Qt::KeyboardModifiers modifiers) {
        const QPointF position(1.0, 1.0);
        QMouseEvent event(
            QEvent::MouseButtonPress, position, position, position,
            Qt::MiddleButton, Qt::MiddleButton, modifiers);
        QCoreApplication::sendEvent(&pane, &event);
        QVERIFY(event.isAccepted());
    };

    pressMiddleButton(Qt::ShiftModifier);
    QCOMPARE(pasted.count(), 1);
    QCOMPARE(pasted.constLast().constFirst().toString(),
             supportsPrimary ? primaryText : standardText);
    QCOMPARE(unsafe.count(), 0);

    LaunchOptions reloaded = options;
    reloaded.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::PrimaryAndClipboard;
    pane.applyRuntimeOptions(reloaded);
    pressMiddleButton(Qt::ShiftModifier);
    QCOMPARE(pasted.count(), 2);
    QCOMPARE(pasted.constLast().constFirst().toString(), standardText);
    QCOMPARE(unsafe.count(), 0);

    pressMiddleButton(Qt::NoModifier);
    QCOMPARE(mouse.count(), 1);
    QCOMPARE(pasted.count(), 2);
    QCOMPARE(unsafe.count(), 0);

    // The user policy restores ordinary middle-click behavior even while the
    // terminal's raw DEC capture request remains active.
    reloaded.mouseReporting = false;
    pane.applyRuntimeOptions(reloaded);
    QVERIFY(controller->terminalMouseTracking());
    QVERIFY(!controller->mouseTracking());
    pressMiddleButton(Qt::NoModifier);
    QCOMPARE(mouse.count(), 1);
    QCOMPARE(pasted.count(), 3);
    QCOMPARE(pasted.constLast().constFirst().toString(), standardText);
    QCOMPARE(unsafe.count(), 0);

    reloaded.middleClickAction = MiddleClickAction::Ignore;
    pane.applyRuntimeOptions(reloaded);
    pressMiddleButton(Qt::NoModifier);
    QCOMPARE(pasted.count(), 3);
    QCOMPARE(unsafe.count(), 0);

    clipboard->clear(QClipboard::Clipboard);
    if (supportsPrimary) {
        clipboard->clear(QClipboard::Selection);
    }
}

void TerminalPaneTest::togglesMouseReportingPolicyAcrossGesturesAndReloads()
{
    qRegisterMetaType<TerminalMouseInput>();
    qRegisterMetaType<TerminalUpdate>();

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/mouse-reporting-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString enableMode =
        directory.filePath(QStringLiteral("enable-mode"));
    const QString disableMode =
        directory.filePath(QStringLiteral("disable-mode"));
    const QString reenableMode =
        directory.filePath(QStringLiteral("reenable-mode"));

    LaunchOptions configured;
    configured.workingDirectory = directory.path();
    configured.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty -echo; printf 'raw-off-ready\\r\\nmouse-target'; "
            "while [ ! -e \"$1\" ]; do sleep 0.01; done; "
            "printf '\\033[?1002h\\r\\nraw-on-ready'; "
            "while [ ! -e \"$2\" ]; do sleep 0.01; done; "
            "printf '\\033[?1002l\\r\\nraw-off-again-ready'; "
            "while [ ! -e \"$3\" ]; do sleep 0.01; done; "
            "printf '\\033[?1002h\\r\\nraw-on-again-ready'; "
            "exec cat >/dev/null"),
        QStringLiteral("mouse-reporting-test"),
        enableMode,
        disableMode,
        reenableMode,
    };
    configured.hold = true;
    configured.mouseReporting = false;
    configured.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;

    TerminalPane pane(configured);
    pane.setSize(QSizeF(320.0, 160.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy mouse(controller, &TerminalController::mouseRequested);
    QSignalSpy scroll(controller, &TerminalController::scrollRequested);
    QSignalSpy selectionBegin(
        controller, &TerminalController::beginSelectionRequested);
    QSignalSpy selectionUpdate(
        controller, &TerminalController::updateSelectionRequested);
    QSignalSpy selectionEnd(
        controller, &TerminalController::endSelectionRequested);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("raw-off-ready")), 5000);
    QVERIFY(!controller->terminalMouseTracking());
    QVERIFY(!controller->mouseReportingEnabled());
    QVERIFY(!controller->mouseTracking());

    const QPointF start(8.0, 8.0);
    const QPointF moved(24.0, 8.0);
    const auto sendMouse = [&pane](QEvent::Type type,
                                   const QPointF &position,
                                   Qt::MouseButton button,
                                   Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, position, position,
                          button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&pane, &event);
        QVERIFY(event.isAccepted());
    };
    const auto sendGesture = [&] {
        sendMouse(QEvent::MouseButtonPress, start,
                  Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseMove, moved,
                  Qt::NoButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, moved,
                  Qt::LeftButton, Qt::NoButton);
    };
    const auto sendWheel = [&pane, &start](
                               Qt::KeyboardModifiers modifiers =
                                   Qt::NoModifier) {
        QWheelEvent event(start, start, QPoint{}, QPoint(0, 120),
                          Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&pane, &event);
        QVERIFY(event.isAccepted());
    };
    const auto touch = [](const QString &path) {
        QFile marker(path);
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
    };

    // Config false plus raw DEC mode false is local selection.
    sendGesture();
    QCOMPARE(mouse.count(), 0);
    QCOMPARE(selectionBegin.count(), 1);
    QCOMPARE(selectionUpdate.count(), 1);
    QCOMPARE(selectionEnd.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(controller->selectionAvailable(), 1000);

    // Policy true alone cannot capture while the terminal has not requested
    // a DEC mouse mode.
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(controller->mouseReportingEnabled());
    QVERIFY(!controller->mouseTracking());
    sendGesture();
    QCOMPARE(mouse.count(), 0);
    QCOMPARE(selectionBegin.count(), 2);
    QCOMPARE(selectionUpdate.count(), 2);
    QCOMPARE(selectionEnd.count(), 2);

    // DECSET activates capture only when the independent pane policy is on.
    touch(enableMode);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("raw-on-ready")), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->terminalMouseTracking(), 1000);
    QVERIFY(controller->mouseTracking());
    sendGesture();
    QCOMPARE(mouse.count(), 3);
    QCOMPARE(selectionBegin.count(), 2);
    QCOMPARE(selectionUpdate.count(), 2);
    QCOMPARE(selectionEnd.count(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->selectionAvailable(), 1000);

    // Wheel reporting ignores Shift upstream, emits protocol button four,
    // suppresses viewport scrolling, and clears an existing selection.
    controller->selectAll();
    QTRY_VERIFY_WITH_TIMEOUT(controller->selectionAvailable(), 1000);
    sendWheel(Qt::ShiftModifier);
    QCOMPARE(mouse.count(), 4);
    QCOMPARE(scroll.count(), 0);
    const TerminalMouseInput wheelInput = qvariant_cast<TerminalMouseInput>(
        mouse.constLast().constFirst());
    QCOMPARE(wheelInput.action, TerminalMouseInput::Press);
    QCOMPARE(wheelInput.button, 4);
    QVERIFY(wheelInput.modifiers & Qt::ShiftModifier);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->selectionAvailable(), 1000);

    // Ghostty reevaluates capture for every event. Disabling after a reported
    // press suppresses its later motion/release and does not begin selection.
    sendMouse(QEvent::MouseButtonPress, start,
              Qt::LeftButton, Qt::LeftButton);
    QCOMPARE(mouse.count(), 5);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(!controller->mouseTracking());
    sendMouse(QEvent::MouseMove, moved,
              Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, moved,
              Qt::LeftButton, Qt::NoButton);
    QCOMPARE(mouse.count(), 5);
    QCOMPARE(selectionBegin.count(), 2);
    QCOMPARE(selectionUpdate.count(), 2);
    QCOMPARE(selectionEnd.count(), 2);

    // With raw DEC tracking still requested but policy off, the wheel returns
    // to typed viewport scrolling.
    sendWheel();
    QCOMPARE(mouse.count(), 5);
    QCOMPARE(scroll.count(), 1);
    const TerminalViewportRequest viewport =
        qvariant_cast<TerminalViewportRequest>(
            scroll.constLast().constFirst());
    QCOMPARE(viewport.kind, TerminalViewportRequest::Kind::Delta);
    QCOMPARE(viewport.delta, -3);

    // Conversely, enabling after a local press lets later events take the
    // newly effective remote path. The local gesture still ends before the
    // reported release atomically clears its selection on the worker.
    sendMouse(QEvent::MouseButtonPress, start,
              Qt::LeftButton, Qt::LeftButton);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(controller->mouseTracking());
    sendMouse(QEvent::MouseMove, moved,
              Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, moved,
              Qt::LeftButton, Qt::NoButton);
    QCOMPARE(mouse.count(), 7);
    QCOMPARE(selectionBegin.count(), 3);
    QCOMPARE(selectionUpdate.count(), 2);
    QCOMPARE(selectionEnd.count(), 3);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->selectionAvailable(), 1000);

    // DECRST suppresses capture without changing policy. Toggling policy in
    // this state still cannot capture; restoring DECSET later makes the
    // conjunction effective again.
    touch(disableMode);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("raw-off-again-ready")), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->terminalMouseTracking(), 1000);
    QVERIFY(controller->mouseReportingEnabled());
    QVERIFY(!controller->mouseTracking());
    sendGesture();
    QCOMPARE(mouse.count(), 7);
    QCOMPARE(selectionBegin.count(), 4);
    QCOMPARE(selectionUpdate.count(), 3);
    QCOMPARE(selectionEnd.count(), 4);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(!controller->mouseReportingEnabled());
    QVERIFY(!controller->mouseTracking());
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(controller->mouseReportingEnabled());
    QVERIFY(!controller->mouseTracking());

    touch(reenableMode);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("raw-on-again-ready")), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->terminalMouseTracking(), 1000);
    QVERIFY(controller->mouseTracking());
    sendGesture();
    QCOMPARE(mouse.count(), 10);
    QCOMPARE(selectionBegin.count(), 4);
    QCOMPARE(selectionUpdate.count(), 3);
    QCOMPARE(selectionEnd.count(), 4);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->selectionAvailable(), 1000);

    // Reload replaces the local toggle with the newest configured policy in
    // either direction without touching the still-enabled terminal DEC mode.
    pane.applyRuntimeOptions(configured);
    QVERIFY(!controller->mouseReportingEnabled());
    QVERIFY(controller->terminalMouseTracking());
    LaunchOptions enabled = configured;
    enabled.mouseReporting = true;
    pane.applyRuntimeOptions(enabled);
    QVERIFY(controller->mouseTracking());
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(!controller->mouseTracking());
    // This snapshot is unchanged from options_, but it still replaces the
    // independent runtime override.
    pane.applyRuntimeOptions(enabled);
    QVERIFY(controller->mouseTracking());

    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting:")));
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting:false")));
}

void TerminalPaneTest::routesAllPasteEntryPointsThroughController()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 5"),
    };
    options.hold = true;
    options.clipboardPaste.protection = false;
    options.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::PrimaryAndClipboard;
    options.middleClickAction = MiddleClickAction::PrimaryPaste;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);
    QSignalSpy unsafe(&pane, &TerminalPane::unsafePasteRequested);

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);

    auto *nonText = new QMimeData;
    nonText->setData(QStringLiteral("application/octet-stream"),
                     QByteArrayLiteral("not text"));
    clipboard->setMimeData(nonText, QClipboard::Clipboard);
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("paste_from_clipboard")));
    QCOMPARE(pasted.count(), 0);

    auto *emptyText = new QMimeData;
    emptyText->setData(QStringLiteral("text/plain"), QByteArray{});
    clipboard->setMimeData(emptyText, QClipboard::Clipboard);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("paste_from_clipboard")));
    QCOMPARE(pasted.count(), 0);

    clipboard->setText(QStringLiteral("shortcut\npaste"),
                       QClipboard::Clipboard);

    QKeyEvent shortcutPress(
        QEvent::KeyPress, Qt::Key_V,
        Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("V"));
    QCoreApplication::sendEvent(&pane, &shortcutPress);
    QCOMPARE(pasted.count(), 1);
    QCOMPARE(pasted.constLast().constFirst().toString(),
             QStringLiteral("shortcut\npaste"));
    QKeyEvent shortcutRelease(
        QEvent::KeyRelease, Qt::Key_V,
        Qt::ControlModifier | Qt::ShiftModifier);
    QCoreApplication::sendEvent(&pane, &shortcutRelease);

    clipboard->setText(QStringLiteral("configured\npaste"),
                       QClipboard::Clipboard);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("paste_from_clipboard")));
    QCOMPARE(pasted.count(), 2);
    QCOMPARE(pasted.constLast().constFirst().toString(),
             QStringLiteral("configured\npaste"));

    if (clipboard->supportsSelection()) {
        clipboard->setText(QStringLiteral("standard must not be used"),
                           QClipboard::Clipboard);
        auto *nonTextSelection = new QMimeData;
        nonTextSelection->setData(
            QStringLiteral("application/octet-stream"),
            QByteArrayLiteral("not text"));
        clipboard->setMimeData(nonTextSelection, QClipboard::Selection);
        QVERIFY(!pane.executeConfiguredAction(
            QStringLiteral("paste_from_selection")));
        QCOMPARE(pasted.count(), 2);

        auto *emptySelection = new QMimeData;
        emptySelection->setData(QStringLiteral("text/plain"), QByteArray{});
        clipboard->setMimeData(emptySelection, QClipboard::Selection);
        QVERIFY(pane.executeConfiguredAction(
            QStringLiteral("paste_from_selection")));
        QCOMPARE(pasted.count(), 2);

        clipboard->setText(QStringLiteral("selection\npaste"),
                           QClipboard::Selection);
        QVERIFY(pane.executeConfiguredAction(
            QStringLiteral("paste_from_selection")));
        QCOMPARE(pasted.count(), 3);
        QCOMPARE(pasted.constLast().constFirst().toString(),
                 QStringLiteral("selection\npaste"));
    } else {
        clipboard->setText(QStringLiteral("standard must not be used"),
                           QClipboard::Clipboard);
        QVERIFY(!pane.executeConfiguredAction(
            QStringLiteral("paste_from_selection")));
    }

    const int beforeMiddleClick = pasted.count();
    clipboard->setText(QStringLiteral("middle\npaste"),
                       QClipboard::Clipboard);
    const QPointF position(1.0, 1.0);
    QMouseEvent middleClick(
        QEvent::MouseButtonPress, position, position, position,
        Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &middleClick);
    QVERIFY(middleClick.isAccepted());
    QCOMPARE(pasted.count(), beforeMiddleClick + 1);
    QCOMPARE(pasted.constLast().constFirst().toString(),
             QStringLiteral("middle\npaste"));

    QTest::qWait(100);
    QCOMPARE(unsafe.count(), 0);
    clipboard->clear(QClipboard::Clipboard);
    if (clipboard->supportsSelection()) {
        clipboard->clear(QClipboard::Selection);
    }
}

void TerminalPaneTest::routesUnsafePasteConfirmationThroughWorker()
{
    ShellEnvironment shell;
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.hold = true;
    options.clipboardPaste.bracketedSafe = false;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);
    QSignalSpy confirmed(controller,
                         &TerminalController::confirmPasteRequested);
    QSignalSpy cancelled(controller,
                         &TerminalController::cancelPasteRequested);
    QSignalSpy unsafe(&pane, &TerminalPane::unsafePasteRequested);
    QSignalSpy activity(controller, &TerminalController::activeProcessChanged);

    QTRY_VERIFY_WITH_TIMEOUT(!controller->activeProcess(), 2000);
    activity.clear();

    const QString rejected = QStringLiteral("cancelled\n");
    pane.pasteText(rejected);
    QCOMPARE(pasted.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(unsafe.count(), 1, 2000);
    const quint64 cancelledId = unsafe.constFirst().at(0).toULongLong();
    QVERIFY(cancelledId != 0);
    QCOMPARE(unsafe.constFirst().at(1).toString(), rejected);
    QCOMPARE(qvariant_cast<TerminalPane *>(unsafe.constFirst().at(2)), &pane);
    QVERIFY(!spyContainsBool(activity, true));

    pane.cancelPaste(cancelledId);
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(cancelled.constFirst().constFirst().toULongLong(), cancelledId);
    QTest::qWait(50);
    QVERIFY(!spyContainsBool(activity, true));

    pane.pasteText(QStringLiteral("\n"));
    QCOMPARE(pasted.count(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(unsafe.count(), 2, 2000);
    const quint64 confirmedId = unsafe.constLast().at(0).toULongLong();
    QVERIFY(confirmedId != 0);
    QVERIFY(confirmedId != cancelledId);
    QVERIFY(!spyContainsBool(activity, true));

    pane.confirmPaste(confirmedId);
    QCOMPARE(confirmed.count(), 1);
    QCOMPARE(confirmed.constFirst().constFirst().toULongLong(), confirmedId);
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(activity, true), 2000);
}

void TerminalPaneTest::rendersConfiguredCellCursorAndDecorationAppearance()
{
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 5"),
    };
    options.hold = true;
    options.fontFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;
    options.appearance.palette.fill(Qt::black, 256);
    options.appearance.palette[8] = QColor(QStringLiteral("#00ff00"));
    options.appearance.boldColor.kind = TerminalBoldColorKind::Bright;
    options.appearance.faintOpacity = 0.25;
    options.appearance.selectionBackground.kind = TerminalColorKind::CellForeground;
    options.appearance.selectionForeground.kind = TerminalColorKind::CellBackground;
    options.appearance.searchBackground = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#123456")));
    options.appearance.searchForeground = TerminalColorValue::fromColor(Qt::white);
    options.appearance.searchSelectedBackground = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#654321")));
    options.appearance.searchSelectedForeground =
        TerminalColorValue::fromColor(Qt::white);
    options.appearance.cursorColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#ff00ff")));
    options.appearance.cursorTextColor = TerminalColorValue::fromColor(
        QColor(QStringLiteral("#00ffff")));
    options.appearance.cursorOpacity = 0.5;

    QFont testFont(options.fontFamily);
    testFont.setPointSizeF(options.fontSize);
    testFont.setFixedPitch(true);
    testFont.setStyleHint(QFont::Monospace);
    const QFontMetricsF metrics(testFont);
    const qreal cellWidth = std::max(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max(1.0, std::ceil(metrics.height()));

    constexpr int columns = 12;
    // Leave enough room for a possible process-status overlay at the bottom;
    // appearance samples stay in the first two rows.
    constexpr int rows = 4;
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(qCeil(cellWidth * columns), qCeil(cellHeight * rows));
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);
    QTest::qWait(150);

    TerminalUpdate update;
    update.columns = columns;
    update.rows = rows;
    update.fullFrame = true;
    update.colorsChanged = true;
    update.foreground = Qt::white;
    update.background = Qt::black;
    update.cursorColor = QColor(QStringLiteral("#ff00ff"));
    update.cursorColorExplicit = true;
    update.palette = options.appearance.palette;
    update.cursorChanged = true;
    update.cursorVisible = true;
    update.cursorBlinking = true;
    update.cursorColumn = 3;
    update.cursorRow = 0;
    update.cursorStyle = 1;
    update.cursorColumnSpan = 2;
    update.contentRevision = 77;
    for (int row = 0; row < rows; ++row) {
        TerminalRowUpdate rowUpdate;
        rowUpdate.row = row;
        rowUpdate.cells.resize(columns);
        for (TerminalCell &cell : rowUpdate.cells) {
            cell.foreground = Qt::white;
            cell.background = Qt::black;
            cell.underlineColor = Qt::white;
        }
        update.dirtyRows.append(std::move(rowUpdate));
    }

    TerminalCell &selection = update.dirtyRows[0].cells[0];
    selection.foreground = QColor(QStringLiteral("#ff0000"));
    selection.background = QColor(QStringLiteral("#0000ff"));
    selection.selected = true;

    TerminalCell &bold = update.dirtyRows[0].cells[1];
    bold.text = QString(QChar(0x2588));
    bold.foreground = QColor(QStringLiteral("#800000"));
    bold.bold = true;
    bold.styleForegroundSource = TerminalColorSource::Palette;
    bold.styleForegroundPaletteIndex = 0;

    TerminalCell &faint = update.dirtyRows[0].cells[2];
    faint.text = QString(QChar(0x2588));
    faint.faint = true;

    TerminalCell &cursorHead = update.dirtyRows[0].cells[3];
    cursorHead.text = QStringLiteral("I");
    cursorHead.columnSpan = 2;
    cursorHead.faint = true;
    cursorHead.underlineStyle = TerminalUnderlineStyle::Single;
    cursorHead.underlineUsesForeground = false;
    cursorHead.underlineColor = Qt::red;
    cursorHead.strikeThrough = true;
    cursorHead.overline = true;
    TerminalCell &cursorTail = update.dirtyRows[0].cells[4];
    cursorTail.spacer = true;
    cursorTail.underlineStyle = TerminalUnderlineStyle::Single;
    cursorTail.underlineUsesForeground = false;
    cursorTail.underlineColor = Qt::red;
    cursorTail.strikeThrough = true;
    cursorTail.overline = true;

    const std::array<TerminalUnderlineStyle, 6> underlines{
        TerminalUnderlineStyle::None,
        TerminalUnderlineStyle::Single,
        TerminalUnderlineStyle::Double,
        TerminalUnderlineStyle::Curly,
        TerminalUnderlineStyle::Dotted,
        TerminalUnderlineStyle::Dashed,
    };
    for (int i = 0; i < static_cast<int>(underlines.size()); ++i) {
        TerminalCell &cell = update.dirtyRows[0].cells[6 + i];
        cell.underlineStyle = underlines[static_cast<size_t>(i)];
        cell.underlineUsesForeground = false;
    }

    TerminalCell &invisible = update.dirtyRows[1].cells[0];
    invisible.underlineStyle = TerminalUnderlineStyle::Single;
    invisible.underlineUsesForeground = false;
    invisible.invisible = true;
    TerminalCell &retainedBlink = update.dirtyRows[1].cells[1];
    retainedBlink.text = QString(QChar(0x2588));
    retainedBlink.foreground = QColor(QStringLiteral("#ffff00"));
    retainedBlink.textBlink = true;

    TerminalCell &selectedOverSearch = update.dirtyRows[1].cells[4];
    selectedOverSearch.foreground = QColor(QStringLiteral("#aa0000"));
    selectedOverSearch.background = QColor(QStringLiteral("#0000aa"));
    selectedOverSearch.selected = true;
    TerminalCell &searchWideHead = update.dirtyRows[1].cells[5];
    searchWideHead.columnSpan = 2;
    TerminalCell &searchWideTail = update.dirtyRows[1].cells[6];
    searchWideTail.spacer = true;

    controller->terminalUpdated(update);
    TerminalSearchUpdate searchUpdate;
    searchUpdate.generation = 1;
    searchUpdate.contentRevision = update.contentRevision;
    searchUpdate.active = true;
    searchUpdate.complete = true;
    searchUpdate.totalMatches = 2;
    searchUpdate.selectedMatch = 0;
    searchUpdate.columns = columns;
    searchUpdate.rows = rows;
    searchUpdate.visibleCellMask = cellMask(columns, rows, {
        QPoint(2, 1), QPoint(3, 1), QPoint(4, 1), QPoint(5, 1),
    });
    searchUpdate.selectedCellMask = cellMask(
        columns, rows, {QPoint(3, 1), QPoint(4, 1)});
    controller->searchUpdated(searchUpdate);
    // The synthetic frame was delivered synchronously. Keep a later PTY
    // readiness update from replacing it while the software scene graph is
    // being sampled.
    QObject::disconnect(controller, &TerminalController::terminalUpdated,
                        pane, nullptr);
    QTest::qWait(100);
    const QImage image = window.grabWindow();
    QVERIFY(!image.isNull());

    const qreal xScale = static_cast<qreal>(image.width()) / window.width();
    const qreal yScale = static_cast<qreal>(image.height()) / window.height();
    const auto centerColor = [&](int column) {
        return image.pixelColor(
            qBound(0, qRound((column + 0.5) * cellWidth * xScale), image.width() - 1),
            qBound(0, qRound(0.5 * cellHeight * yScale), image.height() - 1));
    };

    const QColor selectionPixel = centerColor(0);
    QVERIFY2(approximatelyEqual(selectionPixel, QColor(QStringLiteral("#ff0000"))),
             qPrintable(QStringLiteral("selection pixel=%1 image=%2x%3 cell=%4x%5")
                            .arg(selectionPixel.name(QColor::HexArgb))
                            .arg(image.width()).arg(image.height())
                            .arg(cellWidth).arg(cellHeight)));
    QVERIFY(approximatelyEqual(centerColor(1), QColor(QStringLiteral("#00ff00"))));
    const QColor faintPixel = centerColor(2);
    QVERIFY(faintPixel.red() >= 55 && faintPixel.red() <= 75);
    QVERIFY(faintPixel.green() >= 55 && faintPixel.green() <= 75);
    QVERIFY(faintPixel.blue() >= 55 && faintPixel.blue() <= 75);
    bool foundCursorBackground = false;
    bool foundCursorText = false;
    const int cursorLeft = qRound(3.0 * cellWidth * xScale);
    const int cursorRight = qRound(5.0 * cellWidth * xScale);
    for (int y = 0; y < qRound(cellHeight * yScale); ++y) {
        for (int x = cursorLeft; x < cursorRight; ++x) {
            const QColor pixel = image.pixelColor(x, y);
            foundCursorBackground = foundCursorBackground
                || (pixel.red() >= 120 && pixel.red() <= 140
                    && pixel.green() <= 10
                    && pixel.blue() >= 120 && pixel.blue() <= 140);
            foundCursorText = foundCursorText
                || (pixel.red() <= 10 && pixel.green() >= 245
                    && pixel.blue() >= 245);
        }
    }
    QVERIFY(foundCursorBackground);
    QVERIFY(foundCursorText);
    bool foundWideCursorDecoration = false;
    for (int y = 0; y < qRound(cellHeight * yScale); ++y) {
        for (int x = qRound(4.0 * cellWidth * xScale);
             x < cursorRight; ++x) {
            const QColor pixel = image.pixelColor(x, y);
            foundWideCursorDecoration = foundWideCursorDecoration
                || (pixel.red() <= 10 && pixel.green() >= 245
                    && pixel.blue() >= 245);
            QVERIFY(!(pixel.red() >= 245 && pixel.green() <= 10
                      && pixel.blue() <= 10));
        }
    }
    QVERIFY(foundWideCursorDecoration);

    std::array<int, 6> decorationPixels{};
    std::array<QSet<int>, 6> decorationRows;
    for (int style = 0; style < 6; ++style) {
        const int left = qRound((6 + style) * cellWidth * xScale);
        const int right = qRound((7 + style) * cellWidth * xScale);
        for (int y = 0; y < qRound(cellHeight * yScale); ++y) {
            for (int x = left; x < right; ++x) {
                if (!approximatelyEqual(image.pixelColor(x, y), Qt::black)) {
                    ++decorationPixels[static_cast<size_t>(style)];
                    decorationRows[static_cast<size_t>(style)].insert(y);
                }
            }
        }
    }
    QCOMPARE(decorationPixels[0], 0);
    QVERIFY(decorationPixels[1] > 0);
    QVERIFY(decorationPixels[2] > decorationPixels[1]);
    QVERIFY(decorationRows[3].size() >= 2);
    QVERIFY(decorationPixels[4] > 0);
    QVERIFY(decorationPixels[4] < decorationPixels[1]);
    QVERIFY(decorationPixels[5] > decorationPixels[4]);
    QVERIFY(decorationPixels[5] < decorationPixels[1]);

    const auto secondRowCenterColor = [&](int column, const QImage &source) {
        return source.pixelColor(
            qBound(0, qRound((column + 0.5) * cellWidth * xScale),
                   source.width() - 1),
            qBound(0, qRound(1.5 * cellHeight * yScale),
                   source.height() - 1));
    };
    int invisiblePixels = 0;
    for (int y = qRound(cellHeight * yScale);
         y < qRound(2.0 * cellHeight * yScale); ++y) {
        for (int x = 0; x < qRound(cellWidth * xScale); ++x) {
            if (!approximatelyEqual(image.pixelColor(x, y), Qt::black)) {
                ++invisiblePixels;
            }
        }
    }
    QCOMPARE(invisiblePixels, 0);
    QVERIFY(approximatelyEqual(secondRowCenterColor(1, image),
                               QColor(QStringLiteral("#ffff00"))));
    QVERIFY(approximatelyEqual(secondRowCenterColor(2, image),
                               QColor(QStringLiteral("#123456"))));
    QVERIFY(approximatelyEqual(secondRowCenterColor(3, image),
                               QColor(QStringLiteral("#654321"))));
    // A normal terminal selection remains stronger than both search layers.
    QVERIFY(approximatelyEqual(secondRowCenterColor(4, image),
                               QColor(QStringLiteral("#aa0000"))));
    QVERIFY(approximatelyEqual(secondRowCenterColor(5, image),
                               QColor(QStringLiteral("#123456"))));
    QVERIFY(approximatelyEqual(secondRowCenterColor(6, image),
                               QColor(QStringLiteral("#123456"))));
    // Cross the 600 ms cursor-blink cadence: SGR text blink remains stable,
    // matching the pinned Ghostty renderer rather than sharing that timer.
    QTest::qWait(650);
    const QImage laterImage = window.grabWindow();
    QVERIFY(!laterImage.isNull());
    QVERIFY(approximatelyEqual(secondRowCenterColor(1, laterImage),
                               QColor(QStringLiteral("#ffff00"))));

    const auto cursorContains = [&](const QImage &source,
                                    const auto &predicate) {
        for (int y = 0; y < qRound(cellHeight * yScale); ++y) {
            for (int x = cursorLeft; x < cursorRight; ++x) {
                if (predicate(source.pixelColor(x, y))) {
                    return true;
                }
            }
        }
        return false;
    };
    QVERIFY(!cursorContains(laterImage, [](const QColor &pixel) {
        return pixel.red() >= 110 && pixel.green() <= 20
            && pixel.blue() >= 110;
    }));

    // Losing focus stops the blink timer and immediately presents Ghostty's
    // opaque hollow cursor. Regaining focus shows the configured cursor at
    // once and restarts its interval.
    QQuickItem focusTarget(window.contentItem());
    focusTarget.setFocusPolicy(Qt::StrongFocus);
    focusTarget.forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(!pane->hasActiveFocus(), 1000);
    QTest::qWait(50);
    const QImage unfocusedImage = window.grabWindow();
    QVERIFY(cursorContains(unfocusedImage, [](const QColor &pixel) {
        return pixel.red() >= 245 && pixel.green() <= 10
            && pixel.blue() >= 245;
    }));

    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);
    QTest::qWait(50);
    const QImage refocusedImage = window.grabWindow();
    QVERIFY(cursorContains(refocusedImage, [](const QColor &pixel) {
        return pixel.red() >= 120 && pixel.red() <= 140
            && pixel.green() <= 10
            && pixel.blue() >= 120 && pixel.blue() <= 140;
    }));

    window.close();
    delete pane;
}

void TerminalPaneTest::routesEmergencyTabShortcuts()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    TerminalPane pane(options);
    QSignalSpy tabChange(&pane, &TerminalPane::requestTabChange);
    QSignalSpy applicationActions(
        &pane, &TerminalPane::applicationActionRequested);

    QKeyEvent newWindow(
        QEvent::KeyPress, Qt::Key_N,
        Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("N"));
    QCoreApplication::sendEvent(&pane, &newWindow);
    QCOMPARE(applicationActions.count(), 1);
    QCOMPARE(qvariant_cast<ApplicationAction>(
                 applicationActions.constFirst().constFirst()),
             ApplicationAction::NewWindow);

    QKeyEvent next(QEvent::KeyPress, Qt::Key_Tab,
                   Qt::ControlModifier, QStringLiteral("\t"));
    QCoreApplication::sendEvent(&pane, &next);
    QCOMPARE(tabChange.count(), 1);
    QCOMPARE(tabChange.constLast().constFirst().toInt(), 1);

    QKeyEvent previous(QEvent::KeyPress, Qt::Key_Backtab,
                       Qt::ControlModifier | Qt::ShiftModifier,
                       QStringLiteral("\t"));
    QCoreApplication::sendEvent(&pane, &previous);
    QCOMPARE(tabChange.count(), 2);
    QCOMPARE(tabChange.constLast().constFirst().toInt(), -1);
}

void TerminalPaneTest::routesConfiguredBindingsAndDisablesEmergencyFallback()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindings = {
        QStringLiteral("alt+n=new_tab"),
        QStringLiteral("ctrl+x=increase_font_size:2.5"),
        QStringLiteral("ctrl+enter=toggle_fullscreen"),
        QStringLiteral("alt+m=toggle_maximize"),
        QStringLiteral("ctrl+w=close_tab:this"),
        QStringLiteral("ctrl+shift+w=close_surface"),
        QStringLiteral("ctrl+r=reload_config"),
        QStringLiteral("alt+f4=close_window"),
        QStringLiteral("ctrl+y=open_config"),
        QStringLiteral("ctrl+b=copy_to_clipboard:mixed"),
        QStringLiteral("unconsumed:ctrl+l=reload_config"),
        QStringLiteral("unconsumed:ctrl+i=ignore"),
        QStringLiteral("performable:ctrl+g=goto_split:left"),
        QStringLiteral("performable:ctrl+j=open_config"),
        QStringLiteral("chain=new_tab"),
        QStringLiteral("ctrl+k=close_tab:right"),
        QStringLiteral("chain=new_tab"),
        QStringLiteral("ctrl+o=quit"),
        QStringLiteral("chain=ignore"),
        QStringLiteral("performable:ctrl+c=copy_to_clipboard:mixed"),
        QStringLiteral("performable:ctrl+d=copy_title_to_clipboard"),
        QStringLiteral("performable:alt+e=end_search"),
        QStringLiteral("unconsumed:ctrl+h=close_tab:right"),
        QStringLiteral("chain=ignore"),
        QStringLiteral("alt+t=copy_title_to_clipboard"),
        QStringLiteral("chain=paste_from_clipboard"),
    };

    TerminalPane pane(options);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);
    QSignalSpy closeSurface(&pane, &TerminalPane::requestClose);
    QSignalSpy closeTab(&pane, &TerminalPane::requestCloseTab);
    QSignalSpy applicationActions(
        &pane, &TerminalPane::applicationActionRequested);
    QSignalSpy closeWindowRequested(&pane,
                                    &TerminalPane::requestCloseWindow);
    const auto applicationActionCount = [&applicationActions](
                                            ApplicationAction action) {
        return static_cast<int>(std::ranges::count_if(
            applicationActions, [action](const QList<QVariant> &arguments) {
                return qvariant_cast<ApplicationAction>(arguments.constFirst())
                    == action;
            }));
    };
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy copied(controller, &TerminalController::copyRequested);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);

    QKeyEvent configuredNewTab(QEvent::KeyPress, Qt::Key_N,
                               Qt::AltModifier, QStringLiteral("n"));
    QCoreApplication::sendEvent(&pane, &configuredNewTab);
    QCOMPARE(newTab.count(), 1);

    // Once the flattened Ghostty set is available, an absent binding must
    // not fall through to the emergency hard-coded shortcuts: it may have
    // been explicitly unbound by the user's configuration.
    QKeyEvent unboundEmergency(
        QEvent::KeyPress, Qt::Key_T,
        Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("T"));
    QCoreApplication::sendEvent(&pane, &unboundEmergency);
    QCOMPARE(newTab.count(), 1);

    QKeyEvent zoom(QEvent::KeyPress, Qt::Key_X,
                   Qt::ControlModifier, QStringLiteral("x"));
    QCoreApplication::sendEvent(&pane, &zoom);
    QCOMPARE(pane.fontPointSize(), 14.5);

    QKeyEvent closePane(QEvent::KeyPress, Qt::Key_W,
                        Qt::ControlModifier | Qt::ShiftModifier,
                        QStringLiteral("W"));
    QCoreApplication::sendEvent(&pane, &closePane);
    QCOMPARE(closeSurface.count(), 1);
    QCOMPARE(closeTab.count(), 0);

    QKeyEvent close(QEvent::KeyPress, Qt::Key_W,
                    Qt::ControlModifier, QStringLiteral("w"));
    QCoreApplication::sendEvent(&pane, &close);
    QCOMPARE(closeTab.count(), 1);
    QCOMPARE(qvariant_cast<CloseTabMode>(
                 closeTab.constFirst().constFirst()),
             CloseTabMode::This);

    QKeyEvent reloadEvent(QEvent::KeyPress, Qt::Key_R,
                          Qt::ControlModifier, QStringLiteral("r"));
    QCoreApplication::sendEvent(&pane, &reloadEvent);
    QCOMPARE(applicationActionCount(ApplicationAction::ReloadConfig), 1);

    QKeyEvent closeWindow(QEvent::KeyPress, Qt::Key_F4,
                          Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &closeWindow);
    QCOMPARE(closeWindowRequested.count(), 1);
    QCOMPARE(applicationActionCount(ApplicationAction::Quit), 0);

    const int beforeOpenConfig = forwarded.count();
    QKeyEvent openConfig(QEvent::KeyPress, Qt::Key_Y,
                         Qt::ControlModifier, QString(QChar(0x19)));
    QCoreApplication::sendEvent(&pane, &openConfig);
    QCOMPARE(applicationActionCount(ApplicationAction::OpenConfig), 1);
    QCOMPARE(forwarded.count(), beforeOpenConfig);

    // A normal consumed binding suppresses terminal input even when its
    // state-dependent action has nothing to copy.
    QKeyEvent consumedEmptyCopy(QEvent::KeyPress, Qt::Key_B,
                                Qt::ControlModifier, QString(QChar(0x02)));
    QCoreApplication::sendEvent(&pane, &consumedEmptyCopy);
    QCOMPARE(forwarded.count(), beforeOpenConfig);
    QCOMPARE(copied.count(), 0);
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("copy_to_clipboard")));
    QCOMPARE(copied.count(), 0);

    // Unconsumed actions still run, then allow normal VT encoding.
    const int beforeUnconsumedReload = forwarded.count();
    QKeyEvent unconsumedReload(QEvent::KeyPress, Qt::Key_L,
                               Qt::ControlModifier, QString(QChar(0x0c)));
    QCoreApplication::sendEvent(&pane, &unconsumedReload);
    QCOMPARE(applicationActionCount(ApplicationAction::ReloadConfig), 2);
    QCOMPARE(forwarded.count(), beforeUnconsumedReload + 1);

    // Ghostty's ignore action always suppresses encoding, even if the
    // binding itself carries the unconsumed flag.
    const int beforeIgnore = forwarded.count();
    QKeyEvent ignored(QEvent::KeyPress, Qt::Key_I,
                      Qt::ControlModifier, QString(QChar(0x09)));
    QCoreApplication::sendEvent(&pane, &ignored);
    QCOMPARE(forwarded.count(), beforeIgnore);

    // A performable chain executes the application action and its supported
    // surface remainder before reporting success.
    QKeyEvent partialChain(QEvent::KeyPress, Qt::Key_J,
                           Qt::ControlModifier, QString(QChar(0x0a)));
    QCoreApplication::sendEvent(&pane, &partialChain);
    QCOMPARE(applicationActionCount(ApplicationAction::OpenConfig), 2);
    QCOMPARE(newTab.count(), 2);

    // Ghostty executes the complete chain before reporting a surface-closing
    // outcome. Workspace removal is deferred, so the later action is safe.
    QKeyEvent closingChain(QEvent::KeyPress, Qt::Key_K,
                           Qt::ControlModifier, QString(QChar(0x0b)));
    QCoreApplication::sendEvent(&pane, &closingChain);
    QCOMPARE(closeTab.count(), 2);
    QCOMPARE(qvariant_cast<CloseTabMode>(
                 closeTab.constLast().constFirst()),
             CloseTabMode::Right);
    QCOMPARE(newTab.count(), 3);

    // `quit` is not one of Ghostty's closing-surface actions. A following
    // ignore still runs and therefore leaves the release unsuppressed.
    const int beforeQuitChain = forwarded.count();
    QKeyEvent quitChain(QEvent::KeyPress, Qt::Key_O,
                        Qt::ControlModifier, QString(QChar(0x0f)));
    QCoreApplication::sendEvent(&pane, &quitChain);
    QCOMPARE(applicationActionCount(ApplicationAction::Quit), 1);
    QCOMPARE(forwarded.count(), beforeQuitChain);
    QKeyEvent quitRelease(QEvent::KeyRelease, Qt::Key_O,
                          Qt::ControlModifier, QString(QChar(0x0f)));
    QCoreApplication::sendEvent(&pane, &quitRelease);
    QCOMPARE(forwarded.count(), beforeQuitChain + 1);

    // A performable copy without a selection is not performed and therefore
    // falls through to terminal input.
    const int beforeEmptyCopy = forwarded.count();
    QKeyEvent emptyCopy(QEvent::KeyPress, Qt::Key_C,
                        Qt::ControlModifier, QString(QChar(0x03)));
    QCoreApplication::sendEvent(&pane, &emptyCopy);
    QCOMPARE(forwarded.count(), beforeEmptyCopy + 1);
    QCOMPARE(copied.count(), 0);

    // A performable action can still have mandatory cleanup. Empty retained
    // search text opens only the UI, so end_search closes that UI while
    // reporting not performed and allowing both key events through.
    pane.setSearchUiText(QString{});
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("start_search")));
    QVERIFY(pane.searchUiActive());
    const int beforeCleanup = forwarded.count();
    QKeyEvent cleanupPress(QEvent::KeyPress, Qt::Key_E,
                           Qt::AltModifier, QStringLiteral("e"));
    QCoreApplication::sendEvent(&pane, &cleanupPress);
    QVERIFY(!pane.searchUiActive());
    QCOMPARE(forwarded.count(), beforeCleanup + 1);
    QKeyEvent cleanupRelease(QEvent::KeyRelease, Qt::Key_E,
                             Qt::AltModifier, QStringLiteral("e"));
    QCoreApplication::sendEvent(&pane, &cleanupRelease);
    QCOMPARE(forwarded.count(), beforeCleanup + 2);

    // The title action follows the same performable contract: absent or
    // explicit-empty raw titles pass through, while a non-empty base consumes
    // the key and synchronously writes the standard clipboard.
    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const int beforeEmptyTitleCopy = forwarded.count();
    QKeyEvent emptyTitleCopy(QEvent::KeyPress, Qt::Key_D,
                             Qt::ControlModifier, QString(QChar(0x04)));
    QCoreApplication::sendEvent(&pane, &emptyTitleCopy);
    QCOMPARE(forwarded.count(), beforeEmptyTitleCopy + 1);

    pane.setSurfaceTitle(QString{});
    QKeyEvent explicitEmptyTitleCopy(
        QEvent::KeyPress, Qt::Key_D,
        Qt::ControlModifier, QString(QChar(0x04)));
    QCoreApplication::sendEvent(&pane, &explicitEmptyTitleCopy);
    QCOMPARE(forwarded.count(), beforeEmptyTitleCopy + 2);

    const QString performableTitle = QStringLiteral("performable title");
    pane.setSurfaceTitle(performableTitle);
    clipboard->setText(QStringLiteral("performable sentinel"),
                       QClipboard::Clipboard);
    QKeyEvent titleCopy(QEvent::KeyPress, Qt::Key_D,
                        Qt::ControlModifier, QString(QChar(0x04)));
    QCoreApplication::sendEvent(&pane, &titleCopy);
    QCOMPARE(forwarded.count(), beforeEmptyTitleCopy + 2);
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard), performableTitle);
    clipboard->clear(QClipboard::Clipboard);

    // Performability comes from the typed workspace result, not merely from
    // emitting an action request. With no pane to the left, the binding acts
    // as absent and reaches the terminal.
    QVector<WorkspaceActionRequest> workspaceRequests;
    pane.setWorkspaceActionHandler(
        [&workspaceRequests](WorkspaceActionRequest request) {
            workspaceRequests.append(request);
            return request.action != WorkspaceAction::NavigatePane
                && request.action != WorkspaceAction::CloseTab;
        });

    // Closing is a chain input effect, not the close handler's return value.
    // The later ignore performs, both actions run, and close takes precedence
    // over ignore so neither press nor release reaches the PTY.
    const int beforeRejectedClose = forwarded.count();
    const int beforeRejectedCloseIgnore =
        applicationActionCount(ApplicationAction::Ignore);
    QKeyEvent rejectedClosePress(QEvent::KeyPress, Qt::Key_H,
                                 Qt::ControlModifier, QString(QChar(0x08)));
    QCoreApplication::sendEvent(&pane, &rejectedClosePress);
    QCOMPARE(workspaceRequests.size(), 1);
    QCOMPARE(workspaceRequests.constLast().action,
             WorkspaceAction::CloseTab);
    QCOMPARE(applicationActionCount(ApplicationAction::Ignore),
             beforeRejectedCloseIgnore + 1);
    QCOMPARE(forwarded.count(), beforeRejectedClose);
    QKeyEvent rejectedCloseRelease(QEvent::KeyRelease, Qt::Key_H,
                                   Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &rejectedCloseRelease);
    QCOMPARE(forwarded.count(), beforeRejectedClose);

    // Mutable GUI state is read at execution, not captured while parsing the
    // chain. The paste sees the title written by the preceding action.
    const QString chainedTitle = QStringLiteral("chain title");
    pane.setSurfaceTitle(chainedTitle);
    clipboard->setText(QStringLiteral("old clipboard"));
    const int beforeTitlePaste = pasted.count();
    QKeyEvent titlePaste(QEvent::KeyPress, Qt::Key_T,
                         Qt::AltModifier, QStringLiteral("t"));
    QCoreApplication::sendEvent(&pane, &titlePaste);
    QCOMPARE(pasted.count(), beforeTitlePaste + 1);
    QCOMPARE(pasted.constLast().constFirst().toString(), chainedTitle);

    const int beforeFullscreen = forwarded.count();
    QKeyEvent fullscreenPress(QEvent::KeyPress, Qt::Key_Return,
                              Qt::ControlModifier, QStringLiteral("\r"));
    QCoreApplication::sendEvent(&pane, &fullscreenPress);
    QCOMPARE(workspaceRequests.size(), 2);
    QCOMPARE(workspaceRequests.constLast().action,
             WorkspaceAction::ToggleFullscreen);
    QCOMPARE(forwarded.count(), beforeFullscreen);
    QKeyEvent fullscreenRelease(QEvent::KeyRelease, Qt::Key_Return,
                                Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &fullscreenRelease);
    QCOMPARE(forwarded.count(), beforeFullscreen);
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("toggle_fullscreen:")));
    QCOMPARE(workspaceRequests.size(), 2);

    const int beforeMaximize = forwarded.count();
    QKeyEvent maximizePress(QEvent::KeyPress, Qt::Key_M,
                            Qt::AltModifier, QStringLiteral("m"));
    QCoreApplication::sendEvent(&pane, &maximizePress);
    QCOMPARE(workspaceRequests.size(), 3);
    QCOMPARE(workspaceRequests.constLast().action,
             WorkspaceAction::ToggleMaximize);
    QCOMPARE(forwarded.count(), beforeMaximize);
    QKeyEvent maximizeRelease(QEvent::KeyRelease, Qt::Key_M,
                              Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &maximizeRelease);
    QCOMPARE(forwarded.count(), beforeMaximize);
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("toggle_maximize:")));
    QCOMPARE(workspaceRequests.size(), 3);

    const int beforeUnavailableNavigation = forwarded.count();
    QKeyEvent unavailableNavigation(QEvent::KeyPress, Qt::Key_G,
                                    Qt::ControlModifier, QString(QChar(0x07)));
    QCoreApplication::sendEvent(&pane, &unavailableNavigation);
    QCOMPARE(forwarded.count(), beforeUnavailableNavigation + 1);
}

void TerminalPaneTest::routesBroadConfiguredActionEffects()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    const auto controlKey = [](quint32 codepoint) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = GhosttyKeybindCtrl,
        };
    };
    options.keybindConfig.root = {
        GhosttyKeybindDefinition{
            .sequence = {controlKey('u')},
            .actions = {
                QStringLiteral("close_tab:right"),
                QStringLiteral("ignore"),
            },
            .flags = GhosttyKeybindFlags{
                .consumed = false,
                .all = true,
            },
        },
        GhosttyKeybindDefinition{
            .sequence = {controlKey('p')},
            .actions = {QStringLiteral("copy_to_clipboard")},
            .flags = GhosttyKeybindFlags{
                .consumed = false,
                .all = true,
                .performable = true,
            },
        },
    };

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy broadActions(&pane, &TerminalPane::broadActionsRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy copied(controller, &TerminalController::copyRequested);

    // A broad closing action takes precedence over a later ignore action, so
    // the release remains suppressed even though ignore is press-only alone.
    QKeyEvent closePress(QEvent::KeyPress, Qt::Key_U,
                         Qt::ControlModifier, QString(QChar(0x15)));
    QCoreApplication::sendEvent(&pane, &closePress);
    QCOMPARE(broadActions.count(), 1);
    QCOMPARE(broadActions.constFirst().constFirst().toStringList(),
             QStringList({QStringLiteral("close_tab:right"),
                          QStringLiteral("ignore")}));
    QKeyEvent closeRelease(QEvent::KeyRelease, Qt::Key_U,
                           Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &closeRelease);
    QCOMPARE(forwarded.count(), 0);

    // Broad dispatch ignores performable and is considered performed even
    // when the originating pane lacks the action's dynamic state.
    QKeyEvent copyPress(QEvent::KeyPress, Qt::Key_P,
                        Qt::ControlModifier, QString(QChar(0x10)));
    QCoreApplication::sendEvent(&pane, &copyPress);
    QCOMPARE(broadActions.count(), 2);
    QCOMPARE(copied.count(), 0);
    QKeyEvent copyRelease(QEvent::KeyRelease, Qt::Key_P,
                          Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &copyRelease);
    QCOMPARE(forwarded.count(), 0);
}

void TerminalPaneTest::routesTypedCloseTabModes()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalPane pane(options);

    const struct {
        QString serialized;
        CloseTabMode mode;
    } cases[] = {
        {QStringLiteral("close_tab"), CloseTabMode::This},
        {QStringLiteral("close_tab:this"), CloseTabMode::This},
        {QStringLiteral("close_tab:other"), CloseTabMode::Other},
        {QStringLiteral("close_tab:right"), CloseTabMode::Right},
    };

    QSignalSpy fallback(&pane, &TerminalPane::requestCloseTab);
    for (const auto &testCase : cases) {
        QVERIFY2(pane.executeConfiguredAction(testCase.serialized),
                 qPrintable(testCase.serialized));
        QCOMPARE(qvariant_cast<CloseTabMode>(
                     fallback.constLast().constFirst()),
                 testCase.mode);
    }
    QCOMPARE(fallback.count(), 4);
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("close_tab:")));
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("close_tab:other:extra")));
    QCOMPARE(fallback.count(), 4);

    QVector<WorkspaceActionRequest> requests;
    pane.setWorkspaceActionHandler(
        [&requests](WorkspaceActionRequest request) {
            requests.append(request);
            return true;
        });
    for (const auto &testCase : cases) {
        QVERIFY(pane.executeConfiguredAction(testCase.serialized));
        QCOMPARE(requests.constLast().action, WorkspaceAction::CloseTab);
        QCOMPARE(requests.constLast().context.closeTabMode, testCase.mode);
    }
    QCOMPARE(requests.size(), 4);
    QCOMPARE(fallback.count(), 4);
}

void TerminalPaneTest::routesViewportAndSelectionActions()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalViewportRequest>();
    qRegisterMetaType<TerminalSelectionAdjustment>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf 'pane-action-selection'; sleep 5"),
    };
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindings = {
        QStringLiteral("performable:alt+u=scroll_to_selection"),
        QStringLiteral("performable:alt+i=adjust_selection:left"),
        QStringLiteral("alt+y=select_all"),
        QStringLiteral("chain=adjust_selection:right"),
        QStringLiteral("chain=scroll_to_selection"),
    };

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy scrolls(controller, &TerminalController::scrollRequested);
    QSignalSpy selectAll(controller, &TerminalController::selectAllRequested);
    QSignalSpy copied(controller, &TerminalController::copyRequested);
    QSignalSpy adjustments(
        controller, &TerminalController::selectionAdjustmentRequested);

    // Before the first worker frame, page actions use the worker's actual
    // 24-row startup geometry rather than the legacy 20-row fallback.
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("scroll_page_down")));
    QCOMPARE(scrolls.count(), 1);
    QCOMPARE(qvariant_cast<TerminalViewportRequest>(
                 scrolls.constFirst().constFirst()).delta,
             qint64(24));
    scrolls.clear();

    // A resize request becomes the page basis immediately. An older worker
    // frame may still be queued back to the UI at this point.
    QSignalSpy resizes(controller, &TerminalController::resizeRequested);
    pane.setSize(QSizeF(640.0, 320.0));
    QVERIFY(!resizes.isEmpty());
    const int requestedRows = resizes.constLast().at(1).toInt();
    QVERIFY(requestedRows > 0);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("scroll_page_down")));
    QCOMPARE(qvariant_cast<TerminalViewportRequest>(
                 scrolls.constFirst().constFirst()).delta,
             qint64(requestedRows));
    scrolls.clear();

    // Selection-dependent performable bindings act as absent until the
    // worker's cached selection state says they can run.
    QKeyEvent missingScrollSelection(
        QEvent::KeyPress, Qt::Key_U, Qt::AltModifier, QStringLiteral("u"));
    QCoreApplication::sendEvent(&pane, &missingScrollSelection);
    QCOMPARE(forwarded.count(), 1);
    QCOMPARE(scrolls.count(), 0);
    QKeyEvent missingAdjustment(
        QEvent::KeyPress, Qt::Key_I, Qt::AltModifier, QStringLiteral("i"));
    QCoreApplication::sendEvent(&pane, &missingAdjustment);
    QCOMPARE(forwarded.count(), 2);
    QCOMPARE(adjustments.count(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("pane-action-selection")), 5000);

    // Install a deterministic retained frame matching the queued resize. Page
    // actions must use all requested rows, specifically guarding against the
    // former rows-minus-one behavior.
    TerminalUpdate frame;
    frame.columns = 4;
    frame.rows = requestedRows;
    frame.fullFrame = true;
    for (int row = 0; row < frame.rows; ++row) {
        TerminalRowUpdate update;
        update.row = row;
        update.cells.resize(frame.columns);
        frame.dirtyRows.append(std::move(update));
    }
    controller->terminalUpdated(frame);

    const auto requestAt = [&scrolls](int index) {
        return qvariant_cast<TerminalViewportRequest>(
            scrolls.at(index).constFirst());
    };
    const auto executeScroll = [&pane, &scrolls](QStringView action) {
        const int before = scrolls.count();
        const bool performed = pane.executeConfiguredAction(action);
        return performed && scrolls.count() == before + 1;
    };

    QVERIFY(executeScroll(QStringLiteral("scroll_to_top")));
    QCOMPARE(requestAt(0).kind, TerminalViewportRequest::Kind::Top);
    QVERIFY(executeScroll(QStringLiteral("scroll_to_bottom")));
    QCOMPARE(requestAt(1).kind, TerminalViewportRequest::Kind::Bottom);
    QVERIFY(executeScroll(QStringLiteral("scroll_to_row:+1__2")));
    QCOMPARE(requestAt(2).kind, TerminalViewportRequest::Kind::Row);
    QCOMPARE(requestAt(2).row, quint64(12));
    QVERIFY(executeScroll(QStringLiteral("scroll_page_lines:-7")));
    QCOMPARE(requestAt(3).delta, qint64(-7));
    QVERIFY(executeScroll(QStringLiteral("scroll_page_up")));
    QCOMPARE(requestAt(4).delta, -qint64(requestedRows));
    QVERIFY(executeScroll(QStringLiteral("scroll_page_down")));
    QCOMPARE(requestAt(5).delta, qint64(requestedRows));
    QVERIFY(executeScroll(QStringLiteral("scroll_page_fractional:+0.5")));
    QCOMPARE(requestAt(6).delta,
             static_cast<qint64>(0.5F * static_cast<float>(requestedRows)));
    QVERIFY(executeScroll(QStringLiteral("scroll_page_fractional:-0.2")));
    // Binary32 multiplication happens before truncation toward zero.
    QCOMPARE(requestAt(7).delta,
             static_cast<qint64>(-0.2F * static_cast<float>(requestedRows)));

    const int beforeUnsafe = scrolls.count();
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("scroll_page_fractional:2e18")));
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("scroll_page_fractional:inf")));
    QCOMPARE(scrolls.count(), beforeUnsafe);

    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("scroll_to_selection")));
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("adjust_selection:right")));
    QCOMPARE(scrolls.count(), beforeUnsafe);
    QCOMPARE(adjustments.count(), 0);

    // Select-all intent makes its immediately following dependent actions
    // performable while all three requests retain their worker queue order.
    // This preserves Ghostty action chains without a UI round trip.
    QKeyEvent chainedSelection(
        QEvent::KeyPress, Qt::Key_Y, Qt::AltModifier, QStringLiteral("y"));
    QCoreApplication::sendEvent(&pane, &chainedSelection);
    QCOMPARE(selectAll.count(), 1);
    QCOMPARE(adjustments.count(), 1);
    QCOMPARE(scrolls.count(), beforeUnsafe + 1);
    QCOMPARE(requestAt(beforeUnsafe).kind,
             TerminalViewportRequest::Kind::Selection);
    QCOMPARE(qvariant_cast<TerminalSelectionAdjustment>(
                 adjustments.constFirst().constFirst()),
             TerminalSelectionAdjustment::Right);
    QTRY_VERIFY_WITH_TIMEOUT(controller->selectionAvailable(), 3000);

    for (const QString &copyAction : {
             QStringLiteral("copy_to_clipboard"),
             QStringLiteral("copy_to_clipboard:plain"),
             QStringLiteral("copy_to_clipboard:mixed"),
         }) {
        QVERIFY2(pane.executeConfiguredAction(copyAction),
                 qPrintable(copyAction));
    }
    QCOMPARE(copied.count(), 3);

    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("scroll_to_selection")));
    QCOMPARE(scrolls.count(), beforeUnsafe + 2);
    QCOMPARE(requestAt(beforeUnsafe + 1).kind,
             TerminalViewportRequest::Kind::Selection);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("adjust_selection:right")));
    QCOMPARE(adjustments.count(), 2);
    QCOMPARE(qvariant_cast<TerminalSelectionAdjustment>(
                 adjustments.at(1).constFirst()),
             TerminalSelectionAdjustment::Right);

    const int beforeBoundActions = forwarded.count();
    QKeyEvent availableScrollSelection(
        QEvent::KeyPress, Qt::Key_U, Qt::AltModifier, QStringLiteral("u"));
    QCoreApplication::sendEvent(&pane, &availableScrollSelection);
    QKeyEvent availableAdjustment(
        QEvent::KeyPress, Qt::Key_I, Qt::AltModifier, QStringLiteral("i"));
    QCoreApplication::sendEvent(&pane, &availableAdjustment);
    QCOMPARE(forwarded.count(), beforeBoundActions);
    QCOMPARE(scrolls.count(), beforeUnsafe + 3);
    QCOMPARE(adjustments.count(), 3);
}

void TerminalPaneTest::routesTerminalControlActions()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindings = {
        QStringLiteral("alt+c=csi:31m"),
        QStringLiteral("chain=esc:7"),
        QStringLiteral(R"(chain=text:\\x00\\n\\u{1f47b})"),
        QStringLiteral("chain=reset"),
    };

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    QSignalSpy csi(controller, &TerminalController::csiRequested);
    QSignalSpy escape(controller, &TerminalController::escapeRequested);
    QSignalSpy rawText(controller, &TerminalController::rawTextRequested);
    QSignalSpy reset(controller, &TerminalController::resetTerminalRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QStringList order;
    connect(controller, &TerminalController::csiRequested,
            this, [&order](const QByteArray &) { order.append(QStringLiteral("csi")); });
    connect(controller, &TerminalController::escapeRequested,
            this, [&order](const QByteArray &) { order.append(QStringLiteral("esc")); });
    connect(controller, &TerminalController::rawTextRequested,
            this, [&order](const QByteArray &) { order.append(QStringLiteral("text")); });
    connect(controller, &TerminalController::resetTerminalRequested,
            this, [&order] { order.append(QStringLiteral("reset")); });

    QKeyEvent trigger(QEvent::KeyPress, Qt::Key_C,
                      Qt::AltModifier, QStringLiteral("c"));
    QCoreApplication::sendEvent(&pane, &trigger);

    QCOMPARE(order, QStringList({QStringLiteral("csi"), QStringLiteral("esc"),
                                 QStringLiteral("text"), QStringLiteral("reset")}));
    QCOMPARE(csi.count(), 1);
    QCOMPARE(csi.constFirst().constFirst().toByteArray(), QByteArrayLiteral("31m"));
    QCOMPARE(escape.count(), 1);
    QCOMPARE(escape.constFirst().constFirst().toByteArray(), QByteArrayLiteral("7"));
    QCOMPARE(rawText.count(), 1);
    QCOMPARE(rawText.constFirst().constFirst().toByteArray(),
             QByteArrayLiteral(R"(\\x00\\n\\u{1f47b})"));
    QCOMPARE(reset.count(), 1);
    QCOMPARE(forwarded.count(), 0);

    // Literal validation belongs to the worker: an invalid escape remains a
    // performed/consumed action at the pane boundary, while invalid action
    // grammar never emits a worker request.
    QVERIFY(pane.executeConfiguredAction(QStringLiteral(R"(text:\\q)")));
    QCOMPARE(rawText.count(), 2);
    QCOMPARE(rawText.constLast().constFirst().toByteArray(),
             QByteArrayLiteral(R"(\\q)"));
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("csi")));
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("reset:")));
    QCOMPARE(csi.count(), 1);
    QCOMPARE(reset.count(), 1);
}

void TerminalPaneTest::interactsWithOsc8Hyperlinks()
{
    qRegisterMetaType<TerminalUpdate>();
    const QByteArray validUri = QStringLiteral(
        "https://example.test/ghost-👻").toUtf8();
    const QByteArray invalidUri("https://example.test/%ZZ");
    QByteArray output = QByteArrayLiteral("\033]8;id=valid;");
    output += validUri;
    output += QByteArrayLiteral(
        "\033\\A\033[4mB\033[24m\033]8;;\033\\ ");
    output += QByteArrayLiteral("\033]8;id=invalid;");
    output += invalidUri;
    output += QByteArrayLiteral("\033\\C\033]8;;\033\\");
    QByteArray printfFormat = output;
    printfFormat.replace('%', "%%");

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("printf")),
        QString::fromUtf8(printfFormat),
    };
    QVERIFY(!options.program.constFirst().isEmpty());
    options.hold = true;
    options.fontFamily =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;
    options.keybindingsConfigured = true;
    options.keybindings = {
        QStringLiteral("ctrl+y=copy_url_to_clipboard"),
    };

    QFont font(options.fontFamily);
    font.setPointSizeF(options.fontSize);
    font.setFixedPitch(true);
    font.setStyleHint(QFont::Monospace);
    const QFontMetricsF metrics(font);
    const qreal cellWidth = std::max(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max(1.0, std::ceil(metrics.height()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(qCeil(cellWidth * 10), qCeil(cellHeight * 4));
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy resolved(controller, &TerminalController::hyperlinkResolved);
    QSignalSpy hyperlinkQueries(
        controller, &TerminalController::hyperlinkQueryRequested);
    QSignalSpy errors(controller, &TerminalController::errorOccurred);
    QSignalSpy sessionEnded(pane, &TerminalPane::sessionEnded);
    QSignalSpy activationResolved(
        controller, &TerminalController::hyperlinkActivationResolved);
    QSignalSpy selectionBegun(
        controller, &TerminalController::beginSelectionRequested);
    QSignalSpy selectionUpdated(
        controller, &TerminalController::updateSelectionRequested);
    QSignalSpy selectionEnded(
        controller, &TerminalController::endSelectionRequested);

    int openCount = 0;
    QUrl openedUrl;
    pane->setUrlOpener([&](const QUrl &url) {
        ++openCount;
        openedUrl = url;
        return true;
    });

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(sessionEnded.count(), 1, 5000);
    QVERIFY(!pane->isRunning());
    const QString renderedText = frameText(accumulatedFrame(updates));
    QStringList errorMessages;
    for (const QList<QVariant> &arguments : errors) {
        errorMessages.append(arguments.constFirst().toString());
    }
    QVERIFY2(renderedText.contains(QStringLiteral("AB C")),
             qPrintable(QStringLiteral("terminal text: %1 errors: %2 updates: %3")
                            .arg(renderedText, errorMessages.join(u'|'))
                            .arg(updates.count())));
    const TerminalFrame frame = accumulatedFrame(updates);
    QVERIFY(frame.contentRevision != 0);
    QVERIFY(frame.cells.at(0).hasHyperlink);
    QVERIFY(frame.cells.at(1).hasHyperlink);
    QVERIFY(frame.cells.at(3).hasHyperlink);

    const QPointF validPosition(cellWidth * 0.5, cellHeight * 0.5);
    const QPointF validSecondPosition(cellWidth * 1.5, cellHeight * 0.5);
    const QPointF invalidPosition(cellWidth * 3.5, cellHeight * 0.5);
    const auto sendHover = [&](const QPointF &position,
                               const QPointF &oldPosition,
                               Qt::KeyboardModifiers modifiers) {
        QHoverEvent event(QEvent::HoverMove, position, position,
                          oldPosition, modifiers);
        QCoreApplication::sendEvent(pane, &event);
    };
    const auto sendMouse = [&](QEvent::Type type, const QPointF &position,
                               Qt::MouseButton button,
                               Qt::MouseButtons buttons,
                               Qt::KeyboardModifiers modifiers) {
        QMouseEvent event(type, position, position, position, button, buttons,
                          modifiers);
        QCoreApplication::sendEvent(pane, &event);
    };

    sendHover(validPosition, validPosition, Qt::NoModifier);
    QTest::qWait(50);
    const QImage beforeHover = window.grabWindow();
    QVERIFY(!beforeHover.isNull());
    QGuiApplication::clipboard()->setText(QStringLiteral("sentinel"));
    QVERIFY(!pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("sentinel"));

    // A stationary Ctrl press resolves the link without requiring pointer
    // motion. The configured action copies the exact URI bytes.
    QKeyEvent controlPress(QEvent::KeyPress, Qt::Key_Control,
                           Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &controlPress);
    const auto resolvedUri = [&resolved](const QByteArray &uri) {
        return std::any_of(
            resolved.cbegin(), resolved.cend(),
            [&uri](const QList<QVariant> &arguments) {
                return arguments.at(3).toByteArray() == uri;
            });
    };
    QTRY_VERIFY_WITH_TIMEOUT(resolvedUri(validUri), 1000);
    QVERIFY(hyperlinkQueries.count() >= 1);
    QTRY_COMPARE_WITH_TIMEOUT(pane->cursor().shape(),
                              Qt::PointingHandCursor, 1000);

    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QGuiApplication::clipboard()->setText(QStringLiteral("sentinel"));
    QKeyEvent copyPress(QEvent::KeyPress, Qt::Key_Y,
                        Qt::ControlModifier, QStringLiteral("y"));
    QCoreApplication::sendEvent(pane, &copyPress);
    QCOMPARE(QGuiApplication::clipboard()->mimeData()->data(
                 QStringLiteral("text/plain")), validUri);
    QCOMPARE(QGuiApplication::clipboard()->text(),
             QString::fromUtf8(validUri));
    QKeyEvent copyRelease(QEvent::KeyRelease, Qt::Key_Y,
                          Qt::ControlModifier, QStringLiteral("y"));
    QCoreApplication::sendEvent(pane, &copyRelease);

    QVERIFY(!pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard:")));
    QGuiApplication::clipboard()->setText(QStringLiteral("sentinel"));
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QCOMPARE(QGuiApplication::clipboard()->mimeData()->data(
                 QStringLiteral("text/plain")), validUri);

    // Moving within a resolved multi-cell link preserves the URI/mask and
    // does not enqueue another O(link-cells) worker scan.
    const int queryCount = hyperlinkQueries.count();
    sendHover(validSecondPosition, validPosition, Qt::ControlModifier);
    QCOMPARE(hyperlinkQueries.count(), queryCount);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    sendHover(validPosition, validSecondPosition, Qt::ControlModifier);
    QCOMPARE(hyperlinkQueries.count(), queryCount);

    QTest::qWait(50);
    QVERIFY(!pane->linkPreviewText().isEmpty());
    QVERIFY(!pane->linkPreviewRect().isEmpty());
    const QImage afterHover = window.grabWindow();
    QVERIFY(!afterHover.isNull());
    const qreal xScale = static_cast<qreal>(afterHover.width()) / window.width();
    const qreal yScale = static_cast<qreal>(afterHover.height()) / window.height();
    int previewChanges = 0;
    const QRectF previewRect = pane->linkPreviewRect();
    const int previewLeft = std::clamp(
        qFloor(previewRect.left() * xScale), 0, afterHover.width());
    const int previewRight = std::clamp(
        qCeil(previewRect.right() * xScale), 0, afterHover.width());
    const int previewTop = std::clamp(
        qFloor(previewRect.top() * yScale), 0, afterHover.height());
    const int previewBottom = std::clamp(
        qCeil(previewRect.bottom() * yScale), 0, afterHover.height());
    for (int y = previewTop; y < previewBottom; ++y) {
        for (int x = previewLeft; x < previewRight; ++x) {
            if (beforeHover.pixelColor(x, y)
                != afterHover.pixelColor(x, y)) {
                ++previewChanges;
            }
        }
    }
    QVERIFY2(previewChanges > 20, "link preview was not rendered");
    for (int column = 0; column < 2; ++column) {
        int changedPixels = 0;
        const int left = qRound(column * cellWidth * xScale);
        const int right = qRound((column + 1) * cellWidth * xScale);
        const int bottom = qRound(cellHeight * yScale);
        for (int y = 0; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                if (beforeHover.pixelColor(x, y)
                    != afterHover.pixelColor(x, y)) {
                    ++changedPixels;
                }
            }
        }
        QVERIFY2(changedPixels > 0,
                 qPrintable(QStringLiteral("no hover decoration in column %1")
                                .arg(column)));
    }
    int nonLinkChanges = 0;
    const int nonLinkLeft = qRound(2.0 * cellWidth * xScale);
    const int nonLinkRight = qRound(3.0 * cellWidth * xScale);
    const int firstRowBottom = qRound(cellHeight * yScale);
    for (int y = 0; y < firstRowBottom; ++y) {
        for (int x = nonLinkLeft; x < nonLinkRight; ++x) {
            if (beforeHover.pixelColor(x, y)
                != afterHover.pixelColor(x, y)) {
                ++nonLinkChanges;
            }
        }
    }
    QCOMPARE(nonLinkChanges, 0);

    // A zero-row fractional scroll is a worker no-op. It must retain the
    // accepted lease without waiting for a frame or pointer event that will
    // never be produced.
    const int resolvedBeforeNoOp = resolved.count();
    const int queriesBeforeNoOp = hyperlinkQueries.count();
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("scroll_page_fractional:0.001")));
    QTest::qWait(30);
    QCOMPARE(resolved.count(), resolvedBeforeNoOp);
    QCOMPARE(hyperlinkQueries.count(), queriesBeforeNoOp);
    QCOMPARE(pane->cursor().shape(), Qt::PointingHandCursor);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));

    QKeyEvent controlRelease(QEvent::KeyRelease, Qt::Key_Control,
                             Qt::NoModifier);
    QCoreApplication::sendEvent(pane, &controlRelease);
    QVERIFY(!pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));

    // Ghostty opens on release only. A click does not depend on the hover
    // lookup having completed: the raw link bit arms the press and a second
    // worker lookup revalidates the URI/revision on release.
    const int selectionsBeforeClick = selectionBegun.count();
    sendMouse(QEvent::MouseButtonPress, validPosition, Qt::LeftButton,
              Qt::LeftButton, Qt::ControlModifier);
    QCOMPARE(openCount, 0);
    sendMouse(QEvent::MouseButtonRelease, validPosition, Qt::LeftButton,
              Qt::NoButton, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(openCount, 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(activationResolved.count(), 1, 1000);
    QCOMPARE(openedUrl, QUrl::fromEncoded(validUri, QUrl::StrictMode));
    QCOMPARE(selectionBegun.count(), selectionsBeforeClick + 1);
    QCOMPARE(selectionEnded.count(), selectionsBeforeClick + 1);

    // Malformed Qt URLs remain highlightable/copyable raw OSC 8 data, but are
    // never passed to an external opener. Injecting the already worker-tested
    // result here keeps the pane-side state deterministic under offscreen Qt.
    QCoreApplication::sendEvent(pane, &controlPress);
    const int invalidQueriesBefore = hyperlinkQueries.count();
    sendHover(invalidPosition, validPosition, Qt::ControlModifier);
    QCOMPARE(hyperlinkQueries.count(), invalidQueriesBefore + 1);
    const TerminalFrame invalidFrame = accumulatedFrame(updates);
    controller->hyperlinkResolved(
        invalidFrame.contentRevision, TerminalHyperlinkState::Visible,
        TerminalLinkKind::Osc8, invalidUri, QPoint(3, 0), {QPoint(3, 0)});
    QGuiApplication::clipboard()->setText(QStringLiteral("sentinel"));
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QCOMPARE(QGuiApplication::clipboard()->mimeData()->data(
                 QStringLiteral("text/plain")), invalidUri);
    QVERIFY(!pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard:")));
    sendMouse(QEvent::MouseButtonPress, invalidPosition, Qt::LeftButton,
              Qt::LeftButton, Qt::ControlModifier);
    sendMouse(QEvent::MouseButtonRelease, invalidPosition, Qt::LeftButton,
              Qt::NoButton, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(activationResolved.count(), 2, 1000);
    QCOMPARE(openCount, 1);
    QCoreApplication::sendEvent(pane, &controlRelease);

    // A coalesced press/release that moves far enough within one cell is a
    // drag, not a click. The selection gesture still begins and ends.
    QVERIFY(cellHeight - 2.0
            >= QGuiApplication::styleHints()->startDragDistance());
    const QPointF sameCellDragStart(cellWidth * 0.5, 1.0);
    const QPointF sameCellDragEnd(cellWidth * 0.5, cellHeight - 1.0);
    const int selectionsBeforeSameCellDrag = selectionBegun.count();
    sendMouse(QEvent::MouseButtonPress, sameCellDragStart, Qt::LeftButton,
              Qt::LeftButton, Qt::ControlModifier);
    sendMouse(QEvent::MouseButtonRelease, sameCellDragEnd, Qt::LeftButton,
              Qt::NoButton, Qt::ControlModifier);
    QTest::qWait(50);
    QCOMPARE(openCount, 1);
    QCOMPARE(selectionBegun.count(), selectionsBeforeSameCellDrag + 1);
    QCOMPARE(selectionEnded.count(), selectionsBeforeSameCellDrag + 1);

    // Crossing a cell while held cancels activation even when both cells are
    // part of the same OSC 8 link, while selection continues normally.
    const int selectionUpdatesBeforeDrag = selectionUpdated.count();
    sendMouse(QEvent::MouseButtonPress, validPosition, Qt::LeftButton,
              Qt::LeftButton, Qt::ControlModifier);
    sendMouse(QEvent::MouseMove, validSecondPosition, Qt::NoButton,
              Qt::LeftButton, Qt::ControlModifier);
    sendMouse(QEvent::MouseButtonRelease, validSecondPosition,
              Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
    QTest::qWait(50);
    QCOMPARE(openCount, 1);
    QCOMPARE(selectionUpdated.count(), selectionUpdatesBeforeDrag + 1);

    QVERIFY(pane->cursor().shape() != Qt::PointingHandCursor);

    // Focus loss cancels the accepted hover, and a late result cannot
    // resurrect it after cancellation.
    QCoreApplication::sendEvent(pane, &controlPress);
    sendHover(validPosition, validSecondPosition, Qt::ControlModifier);
    const TerminalFrame focusFrame = accumulatedFrame(updates);
    controller->hyperlinkResolved(
        focusFrame.contentRevision, TerminalHyperlinkState::Visible,
        TerminalLinkKind::Osc8, validUri, QPoint(0, 0),
        {QPoint(0, 0), QPoint(1, 0)});
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    const int opensBeforeFocusLoss = openCount;
    sendMouse(QEvent::MouseButtonPress, validPosition, Qt::LeftButton,
              Qt::LeftButton, Qt::ControlModifier);
    sendMouse(QEvent::MouseButtonRelease, validPosition, Qt::LeftButton,
              Qt::NoButton, Qt::ControlModifier);
    QQuickItem focusTarget(window.contentItem());
    focusTarget.setFocusPolicy(Qt::StrongFocus);
    focusTarget.forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(!pane->hasActiveFocus(), 1000);
    QVERIFY(!pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    controller->hyperlinkActivationResolved(
        focusFrame.contentRevision, TerminalLinkKind::Osc8, validUri);
    QTest::qWait(30);
    QCOMPARE(openCount, opensBeforeFocusLoss);
    controller->hyperlinkResolved(
        focusFrame.contentRevision, TerminalHyperlinkState::Visible,
        TerminalLinkKind::Osc8, validUri, QPoint(0, 0),
        {QPoint(0, 0), QPoint(1, 0)});
    QVERIFY(!pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty()
                 ? ""
                 : qPrintable(errors.constFirst().constFirst().toString()));

    window.close();
    delete pane;
}

void TerminalPaneTest::interactsWithRegexLinksAndReloadsLinkUrl()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalHyperlinkState>();
    qRegisterMetaType<TerminalLinkKind>();

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString relativeName = QStringLiteral("relative-link.txt");
    const QString relativePath = directory.filePath(relativeName);
    QFile relativeFile(relativePath);
    QVERIFY(relativeFile.open(QIODevice::WriteOnly));
    QCOMPARE(relativeFile.write("regex-link-target\n"), qint64(18));
    relativeFile.close();

    const QByteArray regexText = QByteArrayLiteral("./relative-link.txt");
    const QByteArray oscUri = QByteArrayLiteral(
        "https://example.test/osc-still-enabled");
    QByteArray output = regexText;
    output += QByteArrayLiteral("\r\n\033]8;;");
    output += oscUri;
    output += QByteArrayLiteral("\033\\OSC\033]8;;\033\\");

    LaunchOptions options;
    options.workingDirectory = directory.path();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("printf")),
        QString::fromUtf8(output),
    };
    QVERIFY(!options.program.constFirst().isEmpty());
    options.hold = true;
    options.linkUrl = false;
    options.fontFamily =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    options.keybindingsConfigured = true;
    options.keybindings = {
        QStringLiteral("ctrl+y=copy_url_to_clipboard"),
    };

    QFont font(options.fontFamily);
    font.setPointSizeF(options.fontSize);
    font.setFixedPitch(true);
    font.setStyleHint(QFont::Monospace);
    const QFontMetricsF metrics(font);
    const qreal cellWidth = std::max(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max(1.0, std::ceil(metrics.height()));

    QQuickWindow window;
    window.resize(qCeil(cellWidth * 30.0), qCeil(cellHeight * 4.0));
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy queries(
        controller, &TerminalController::hyperlinkQueryRequested);
    QSignalSpy resolved(controller, &TerminalController::hyperlinkResolved);
    QSignalSpy activationResolved(
        controller, &TerminalController::hyperlinkActivationResolved);
    QSignalSpy errors(controller, &TerminalController::errorOccurred);
    QSignalSpy sessionEnded(pane, &TerminalPane::sessionEnded);

    int openCount = 0;
    QUrl openedUrl;
    pane->setUrlOpener([&](const QUrl &url) {
        ++openCount;
        openedUrl = url;
        return true;
    });

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    pane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(sessionEnded.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QString::fromUtf8(regexText)), 1000);

    const QPointF regexPosition(cellWidth * 0.5, cellHeight * 0.5);
    const QPointF oscPosition(cellWidth * 0.5, cellHeight * 1.5);
    const auto sendHover = [&](const QPointF &position,
                               const QPointF &oldPosition) {
        QHoverEvent event(QEvent::HoverMove, position, position,
                          oldPosition, Qt::ControlModifier);
        QCoreApplication::sendEvent(pane, &event);
    };
    const auto sendMouse = [&](QEvent::Type type, const QPointF &position,
                               Qt::MouseButton button,
                               Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, position, position, button, buttons,
                          Qt::ControlModifier);
        QCoreApplication::sendEvent(pane, &event);
    };

    QKeyEvent controlPress(QEvent::KeyPress, Qt::Key_Control,
                           Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &controlPress);
    sendHover(regexPosition, regexPosition);
    QTest::qWait(50);
    QCOMPARE(queries.count(), 0);
    QVERIFY(!pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));

    // link-url is live: enabling it must recompute a stationary hover, expose
    // the exact matched bytes to copy, and resolve a relative path against the
    // terminal's current directory before opening it.
    LaunchOptions enabled = options;
    enabled.linkUrl = true;
    pane->applyRuntimeOptions(enabled);
    QTRY_VERIFY_WITH_TIMEOUT(
        !resolved.isEmpty()
            && qvariant_cast<TerminalHyperlinkState>(
                   resolved.constLast().at(1))
                == TerminalHyperlinkState::Visible
            && qvariant_cast<TerminalLinkKind>(
                   resolved.constLast().at(2))
                == TerminalLinkKind::Regex,
        1000);
    QCOMPARE(resolved.constLast().at(3).toByteArray(), regexText);
    QTRY_COMPARE_WITH_TIMEOUT(
        pane->cursor().shape(), Qt::PointingHandCursor, 1000);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QCOMPARE(QGuiApplication::clipboard()->mimeData()->data(
                 QStringLiteral("text/plain")),
             regexText);

    sendMouse(QEvent::MouseButtonPress, regexPosition, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, regexPosition, Qt::LeftButton,
              Qt::NoButton);
    QTRY_COMPARE_WITH_TIMEOUT(activationResolved.count(), 1, 1000);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(
                 activationResolved.constLast().at(1)),
             TerminalLinkKind::Regex);
    QCOMPARE(activationResolved.constLast().at(2).toByteArray(), regexText);
    QCOMPARE(openCount, 1);
    QCOMPARE(openedUrl, QUrl::fromLocalFile(relativePath));

    const int queriesBeforeDisable = queries.count();
    pane->applyRuntimeOptions(options);
    QTRY_COMPARE_WITH_TIMEOUT(pane->cursor().shape(), Qt::ArrowCursor, 1000);
    QVERIFY(!pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QTest::qWait(50);
    QCOMPARE(queries.count(), queriesBeforeDisable);

    // Explicit hyperlinks are not controlled by link-url.
    QTest::mouseMove(&window, oscPosition.toPoint());
    sendHover(oscPosition, regexPosition);
    QTRY_VERIFY_WITH_TIMEOUT(
        !resolved.isEmpty()
            && qvariant_cast<TerminalHyperlinkState>(
                   resolved.constLast().at(1))
                == TerminalHyperlinkState::Visible
            && qvariant_cast<TerminalLinkKind>(
                   resolved.constLast().at(2))
                == TerminalLinkKind::Osc8,
        1000);
    QCOMPARE(resolved.constLast().at(3).toByteArray(), oscUri);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QCOMPARE(QGuiApplication::clipboard()->mimeData()->data(
                 QStringLiteral("text/plain")),
             oscUri);
    QCOMPARE(openCount, 1);
    QVERIFY(errors.isEmpty());

    QKeyEvent controlRelease(QEvent::KeyRelease, Qt::Key_Control,
                             Qt::NoModifier);
    QCoreApplication::sendEvent(pane, &controlRelease);
    window.close();
    delete pane;
}

void TerminalPaneTest::previewsLinksAccordingToPolicyAndBoundsDisplay()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalHyperlinkState>();
    qRegisterMetaType<TerminalLinkKind>();

    const QByteArray oscUri = QByteArrayLiteral(
        "https://example.test/explicit-destination");
    const QByteArray regexUri = QByteArrayLiteral(
        "https://example.test/detected-destination");
    QByteArray output = QByteArrayLiteral("\033]8;;");
    output += oscUri;
    output += QByteArrayLiteral("\033\\OSC\033]8;;\033\\\r\n");
    output += regexUri;

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("sh")),
        QStringLiteral("-c"),
        QStringLiteral("sleep 0.1; printf '%s' \"$1\""),
        QStringLiteral("ghostty-qt-link-preview"),
        QString::fromUtf8(output),
    };
    QVERIFY(!options.program.constFirst().isEmpty());
    options.hold = true;
    options.linkUrl = true;
    options.linkPreviews = LinkPreviewMode::Always;
    options.fontFamily =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();

    QFont font(options.fontFamily);
    font.setPointSizeF(options.fontSize);
    font.setFixedPitch(true);
    font.setStyleHint(QFont::Monospace);
    const QFontMetricsF metrics(font);
    const qreal cellWidth = std::max(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max(1.0, std::ceil(metrics.height()));

    TerminalPane pane(options);
    pane.setSize(QSizeF(cellWidth * 120.0, cellHeight * 5.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy queries(
        controller, &TerminalController::hyperlinkQueryRequested);
    QSignalSpy resolved(controller, &TerminalController::hyperlinkResolved);
    QSignalSpy previewChanged(&pane, &TerminalPane::linkPreviewChanged);
    QSignalSpy activationPreparations(
        controller,
        &TerminalController::hyperlinkActivationPreparationRequested);
    QSignalSpy selections(
        controller, &TerminalController::beginSelectionRequested);
    QSignalSpy sessionEnded(&pane, &TerminalPane::sessionEnded);

    QTRY_COMPARE_WITH_TIMEOUT(sessionEnded.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QString::fromUtf8(regexUri)), 1000);

    const auto sendHover = [&pane](const QPointF &position,
                                   const QPointF &oldPosition) {
        QHoverEvent event(QEvent::HoverMove, position, position,
                          oldPosition, Qt::ControlModifier);
        QCoreApplication::sendEvent(&pane, &event);
    };
    const auto lastResolvedIs = [&resolved](TerminalLinkKind kind,
                                             const QByteArray &uri) {
        return !resolved.isEmpty()
            && qvariant_cast<TerminalHyperlinkState>(
                   resolved.constLast().at(1))
                == TerminalHyperlinkState::Visible
            && qvariant_cast<TerminalLinkKind>(
                   resolved.constLast().at(2))
                == kind
            && resolved.constLast().at(3).toByteArray() == uri;
    };
    const auto previewIsInsidePane = [&pane] {
        const QRectF preview = pane.linkPreviewRect();
        return !preview.isEmpty() && preview.left() >= 0.0
            && preview.top() >= 0.0
            && preview.right() <= pane.width()
            && preview.bottom() <= pane.height();
    };

    QKeyEvent controlPress(QEvent::KeyPress, Qt::Key_Control,
                           Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &controlPress);
    const QPointF oscPosition(cellWidth * 0.5, cellHeight * 0.5);
    const QPointF regexPosition(cellWidth * 0.5, cellHeight * 1.5);
    sendHover(oscPosition, oscPosition);
    QTRY_VERIFY_WITH_TIMEOUT(
        lastResolvedIs(TerminalLinkKind::Osc8, oscUri), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!pane.linkPreviewText().isEmpty(), 1000);
    QVERIFY(previewIsInsidePane());

    // Preview policy is frontend-only. Toggling it must preserve the accepted
    // worker lease, underline/copy behavior, and query count.
    const int oscQueryCount = queries.count();
    const int changesBeforePolicy = previewChanged.count();
    LaunchOptions reloaded = options;
    reloaded.linkPreviews = LinkPreviewMode::Never;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.linkPreviewText(), QString());
    QVERIFY(pane.linkPreviewRect().isEmpty());
    QCOMPARE(queries.count(), oscQueryCount);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));

    reloaded.linkPreviews = LinkPreviewMode::Osc8;
    pane.applyRuntimeOptions(reloaded);
    QVERIFY(!pane.linkPreviewText().isEmpty());
    QVERIFY(previewIsInsidePane());
    QCOMPARE(queries.count(), oscQueryCount);
    QVERIFY(previewChanged.count() >= changesBeforePolicy + 2);

    // Match GTK's guard behavior without introducing an input-owning child:
    // entering the original bottom-left rectangle retains the logical hover
    // and moves the presentation to the opposite edge.
    const QString previewBeforeRelocation = pane.linkPreviewText();
    const QRectF leftPreview = pane.linkPreviewRect();
    QVERIFY(leftPreview.left() < pane.width() / 2.0);
    sendHover(leftPreview.center(), oscPosition);
    QTRY_COMPARE_WITH_TIMEOUT(
        pane.linkPreviewText(), previewBeforeRelocation, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        pane.linkPreviewRect().left() > leftPreview.left(), 1000);
    QVERIFY(previewIsInsidePane());
    QCOMPARE(queries.count(), oscQueryCount);

    // The GTK overlay's left label remains the pointer guard while its
    // transparent copy is replaced on the right. It is non-focusable and has
    // no action of its own; presses in that occupied guard do not leak through
    // to a different terminal cell or activate the retained link remotely.
    QMouseEvent guardPress(
        QEvent::MouseButtonPress, leftPreview.center(), leftPreview.center(),
        leftPreview.center(), Qt::LeftButton, Qt::LeftButton,
        Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &guardPress);
    QMouseEvent guardRelease(
        QEvent::MouseButtonRelease, leftPreview.center(), leftPreview.center(),
        leftPreview.center(), Qt::LeftButton, Qt::NoButton,
        Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &guardRelease);
    QCOMPARE(activationPreparations.count(), 0);
    QCOMPARE(selections.count(), 0);

    // If live policy removes an occupied guard, reconcile the physical
    // terminal cell immediately instead of leaving the retained source link
    // active at a pointer position elsewhere in the pane.
    const int queriesBeforeCapturedDisable = queries.count();
    reloaded.linkPreviews = LinkPreviewMode::Never;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.linkPreviewText(), QString());
    QVERIFY(pane.linkPreviewRect().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(
        queries.count() > queriesBeforeCapturedDisable, 1000);
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));

    // Re-establish the original hover and guard before exercising pointer
    // motion out of it.
    reloaded.linkPreviews = LinkPreviewMode::Osc8;
    pane.applyRuntimeOptions(reloaded);
    sendHover(oscPosition, leftPreview.center());
    QTRY_VERIFY_WITH_TIMEOUT(
        lastResolvedIs(TerminalLinkKind::Osc8, oscUri), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!pane.linkPreviewText().isEmpty(), 1000);
    const int restoredOscQueryCount = queries.count();
    const QRectF restoredLeftPreview = pane.linkPreviewRect();
    sendHover(restoredLeftPreview.center(), oscPosition);
    QTRY_VERIFY_WITH_TIMEOUT(
        pane.linkPreviewRect().left() > restoredLeftPreview.left(), 1000);
    QCOMPARE(queries.count(), restoredOscQueryCount);

    // Leaving the original guard returns to ordinary terminal hit testing.
    // This point is neither the accepted link nor the bottom preview.
    const QPointF plainPosition(cellWidth * 30.5, cellHeight * 2.5);
    sendHover(plainPosition, restoredLeftPreview.center());
    QTRY_VERIFY_WITH_TIMEOUT(pane.linkPreviewText().isEmpty(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        queries.count() > restoredOscQueryCount, 1000);

    // `osc8` suppresses only regex previews; it does not suppress detection,
    // copy, or the tracked hover itself. Enabling all previews is immediate
    // and does not enqueue a replacement lookup.
    sendHover(regexPosition, plainPosition);
    QTRY_VERIFY_WITH_TIMEOUT(
        lastResolvedIs(TerminalLinkKind::Regex, regexUri), 1000);
    QCOMPARE(pane.linkPreviewText(), QString());
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    const int regexQueryCount = queries.count();

    reloaded.linkPreviews = LinkPreviewMode::Always;
    pane.applyRuntimeOptions(reloaded);
    QTRY_VERIFY_WITH_TIMEOUT(!pane.linkPreviewText().isEmpty(), 1000);
    QVERIFY(previewIsInsidePane());
    QCOMPARE(queries.count(), regexQueryCount);

    reloaded.linkPreviews = LinkPreviewMode::Never;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.linkPreviewText(), QString());
    QCOMPARE(queries.count(), regexQueryCount);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));

    reloaded.linkPreviews = LinkPreviewMode::Always;
    pane.applyRuntimeOptions(reloaded);
    QVERIFY(!pane.linkPreviewText().isEmpty());
    QCOMPARE(queries.count(), regexQueryCount);

    QKeyEvent controlRelease(QEvent::KeyRelease, Qt::Key_Control,
                             Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &controlRelease);
    QCOMPARE(pane.linkPreviewText(), QString());
    QVERIFY(pane.linkPreviewRect().isEmpty());

    // The presentation path is bounded and safe for arbitrary OSC 8 bytes.
    // The exact QByteArray remains owned by hyperlink interaction; only the
    // overlay escapes controls and replaces malformed UTF-8.
    QCoreApplication::sendEvent(&pane, &controlPress);
    sendHover(oscPosition, regexPosition);
    QTRY_VERIFY_WITH_TIMEOUT(
        lastResolvedIs(TerminalLinkKind::Osc8, oscUri), 1000);
    QByteArray unsafeUri;
    unsafeUri += QByteArrayLiteral("unsafe:");
    unsafeUri.append('\0');
    unsafeUri += QByteArrayLiteral("\r\n");
    unsafeUri += QString(QChar(0x2028)).toUtf8();
    unsafeUri += QString(QChar(0x202e)).toUtf8();
    unsafeUri.append(char(0xff));
    unsafeUri += QByteArray(5000, 'x');
    const TerminalFrame frame = accumulatedFrame(updates);
    controller->hyperlinkResolved(
        frame.contentRevision, TerminalHyperlinkState::Visible,
        TerminalLinkKind::Osc8, unsafeUri, QPoint(0, 0), {QPoint(0, 0)});

    QTRY_VERIFY_WITH_TIMEOUT(
        pane.linkPreviewText().contains(QStringLiteral("\\x00")), 1000);
    const QString safePreview = pane.linkPreviewText();
    QVERIFY(safePreview.contains(QStringLiteral("\\x0D\\x0A")));
    QVERIFY(safePreview.contains(QStringLiteral("\\u2028")));
    QVERIFY(safePreview.contains(QStringLiteral("\\u202E")));
    QVERIFY(safePreview.contains(QChar(0xfffd)));
    QVERIFY(safePreview.contains(QChar(0x2026)));
    QVERIFY(!safePreview.contains(QChar(0)));
    QVERIFY(!safePreview.contains(u'\r'));
    QVERIFY(!safePreview.contains(u'\n'));
    QVERIFY(!safePreview.contains(QChar(0x2028)));
    QVERIFY(!safePreview.contains(QChar(0x202e)));
    QVERIFY(previewIsInsidePane());
    QVERIFY(pane.linkPreviewRect().width() <= pane.width());
    QVERIFY(pane.linkPreviewRect().height() <= pane.height());

    const qreal previewHeightBeforeZoom = pane.linkPreviewRect().height();
    const int queriesBeforeZoom = queries.count();
    pane.zoomIn();
    QVERIFY(pane.linkPreviewRect().height() > previewHeightBeforeZoom);
    QVERIFY(!pane.linkPreviewText().isEmpty());
    QCOMPARE(queries.count(), queriesBeforeZoom);

    QHoverEvent leave(QEvent::HoverLeave, QPointF(), QPointF(),
                      oscPosition, Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &leave);
    QCOMPARE(pane.linkPreviewText(), QString());
    QVERIFY(pane.linkPreviewRect().isEmpty());
}

void TerminalPaneTest::keepsOsc8InteractionStableAcrossUnrelatedOutput()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalHyperlinkState>();

    const QByteArray uri = QByteArrayLiteral(
        "https://example.test/live-output");
    const QString script = QStringLiteral(
        "printf '\033[2J\033[H\033]8;;https://example.test/live-output"
        "\033\\LINK\033]8;;\033\\'; "
        "i=0; while [ $i -lt 200 ]; do "
        "printf '\0337\033[4;1Htick-%02d\0338' \"$i\"; "
        "i=$((i + 1)); sleep 0.01; done");

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"), script,
    };
    options.hold = true;
    options.fontFamily =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    options.keybindingsConfigured = true;
    options.keybindings = {
        QStringLiteral("ctrl+y=copy_url_to_clipboard"),
    };

    QFont font(options.fontFamily);
    font.setPointSizeF(options.fontSize);
    font.setFixedPitch(true);
    font.setStyleHint(QFont::Monospace);
    const QFontMetricsF metrics(font);
    const qreal cellWidth = std::max(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max(1.0, std::ceil(metrics.height()));

    QQuickWindow window;
    window.resize(qCeil(cellWidth * 12.0), qCeil(cellHeight * 4.0));
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy queries(
        controller, &TerminalController::hyperlinkQueryRequested);
    QSignalSpy resolved(controller, &TerminalController::hyperlinkResolved);
    QSignalSpy activationResolved(
        controller, &TerminalController::hyperlinkActivationResolved);
    QSignalSpy errors(controller, &TerminalController::errorOccurred);

    int openCount = 0;
    pane->setUrlOpener([&](const QUrl &opened) {
        if (opened == QUrl::fromEncoded(uri, QUrl::StrictMode)) {
            ++openCount;
        }
        return true;
    });

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("LINK")), 5000);

    const QPointF linkPosition(cellWidth * 0.5, cellHeight * 0.5);
    QKeyEvent controlPress(QEvent::KeyPress, Qt::Key_Control,
                           Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &controlPress);
    QHoverEvent hover(QEvent::HoverMove, linkPosition, linkPosition,
                      linkPosition, Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &hover);
    QTRY_VERIFY_WITH_TIMEOUT(
        !resolved.isEmpty()
            && qvariant_cast<TerminalHyperlinkState>(
                   resolved.constLast().at(1))
                == TerminalHyperlinkState::Visible,
        1000);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(resolved.constLast().at(2)),
             TerminalLinkKind::Osc8);
    QCOMPARE(resolved.constLast().at(3).toByteArray(), uri);
    QTRY_COMPARE_WITH_TIMEOUT(
        pane->cursor().shape(), Qt::PointingHandCursor, 1000);
    const int queryCount = queries.count();
    const int resolutionCount = resolved.count();
    const quint64 resolvedRevision = accumulatedFrame(updates).contentRevision;

    // Repeated writes to another row advance the broad terminal revision,
    // but they do not affect the tracked target or enqueue another URI scan.
    QTRY_VERIFY_WITH_TIMEOUT(
        accumulatedFrame(updates).contentRevision
            >= resolvedRevision + 5,
        3000);
    QVERIFY(frameText(accumulatedFrame(updates)).contains(
        QStringLiteral("tick-")));
    QCOMPARE(queries.count(), queryCount);
    QCOMPARE(resolved.count(), resolutionCount);
    QCOMPARE(pane->cursor().shape(), Qt::PointingHandCursor);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QCOMPARE(QGuiApplication::clipboard()->mimeData()->data(
                 QStringLiteral("text/plain")),
             uri);

    const quint64 pressRevision = accumulatedFrame(updates).contentRevision;
    QMouseEvent press(QEvent::MouseButtonPress, linkPosition, linkPosition,
                      linkPosition, Qt::LeftButton, Qt::LeftButton,
                      Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &press);
    QTRY_VERIFY_WITH_TIMEOUT(
        accumulatedFrame(updates).contentRevision > pressRevision, 1000);
    QMouseEvent release(QEvent::MouseButtonRelease, linkPosition,
                        linkPosition, linkPosition, Qt::LeftButton,
                        Qt::NoButton, Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &release);
    QTRY_COMPARE_WITH_TIMEOUT(activationResolved.count(), 1, 1000);
    QCOMPARE(openCount, 1);
    QVERIFY(errors.isEmpty());

    QKeyEvent controlRelease(QEvent::KeyRelease, Qt::Key_Control,
                             Qt::NoModifier);
    QCoreApplication::sendEvent(pane, &controlRelease);

    window.close();
    delete pane;
}

void TerminalPaneTest::restoresOsc8HoverAcrossViewportScroll()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalHyperlinkState>();

    const QByteArray uri = QByteArrayLiteral(
        "https://example.test/tracked-scroll");
    QByteArray output = QByteArrayLiteral("\033]8;;");
    output += uri;
    output += QByteArrayLiteral("\033\\LINK\033]8;;\033\\");
    for (int row = 0; row < 40; ++row) {
        output += QByteArrayLiteral("\r\nplain-row-");
        output += QByteArray::number(row).rightJustified(2, '0');
    }

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStandardPaths::findExecutable(QStringLiteral("printf")),
        QString::fromUtf8(output),
    };
    QVERIFY(!options.program.constFirst().isEmpty());
    options.hold = true;
    options.fontFamily =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();

    QFont font(options.fontFamily);
    font.setPointSizeF(options.fontSize);
    font.setFixedPitch(true);
    font.setStyleHint(QFont::Monospace);
    const QFontMetricsF metrics(font);
    const qreal cellWidth = std::max(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max(1.0, std::ceil(metrics.height()));

    TerminalPane pane(options);
    pane.setSize(QSizeF(cellWidth * 12.0, cellHeight * 3.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy queries(
        controller, &TerminalController::hyperlinkQueryRequested);
    QSignalSpy resolved(controller, &TerminalController::hyperlinkResolved);
    QSignalSpy sessionEnded(&pane, &TerminalPane::sessionEnded);

    QTRY_COMPARE_WITH_TIMEOUT(sessionEnded.count(), 1, 5000);
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("scroll_to_top")));
    QTRY_VERIFY_WITH_TIMEOUT(
        frameText(accumulatedFrame(updates)).contains(
            QStringLiteral("LINK")),
        1000);

    QKeyEvent controlPress(QEvent::KeyPress, Qt::Key_Control,
                           Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &controlPress);
    const QPointF linkPosition(cellWidth * 0.5, cellHeight * 0.5);
    QHoverEvent hover(QEvent::HoverMove, linkPosition, linkPosition,
                      linkPosition, Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &hover);
    QTRY_VERIFY_WITH_TIMEOUT(
        !resolved.isEmpty()
            && qvariant_cast<TerminalHyperlinkState>(
                   resolved.constLast().at(1))
                == TerminalHyperlinkState::Visible,
        1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        pane.cursor().shape(), Qt::PointingHandCursor, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!pane.linkPreviewText().isEmpty(), 1000);
    QVERIFY(!pane.linkPreviewRect().isEmpty());
    const int queryCount = queries.count();

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("scroll_to_bottom")));
    QTRY_VERIFY_WITH_TIMEOUT(
        !resolved.isEmpty()
            && qvariant_cast<TerminalHyperlinkState>(
                   resolved.constLast().at(1))
                == TerminalHyperlinkState::Hidden,
        1000);
    QCOMPARE(queries.count(), queryCount);
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QCOMPARE(pane.cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(pane.linkPreviewText(), QString());
    QVERIFY(pane.linkPreviewRect().isEmpty());

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("scroll_to_top")));
    QTRY_VERIFY_WITH_TIMEOUT(
        qvariant_cast<TerminalHyperlinkState>(
            resolved.constLast().at(1))
            == TerminalHyperlinkState::Visible,
        1000);
    QCOMPARE(queries.count(), queryCount);
    QTRY_COMPARE_WITH_TIMEOUT(
        pane.cursor().shape(), Qt::PointingHandCursor, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!pane.linkPreviewText().isEmpty(), 1000);
    QVERIFY(!pane.linkPreviewRect().isEmpty());

    QKeyEvent controlRelease(QEvent::KeyRelease, Qt::Key_Control,
                             Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &controlRelease);
    QCOMPARE(pane.linkPreviewText(), QString());
    QVERIFY(pane.linkPreviewRect().isEmpty());
}

void TerminalPaneTest::letsShiftBypassMouseCaptureForHyperlinks()
{
    qRegisterMetaType<TerminalUpdate>();
    qRegisterMetaType<TerminalMouseInput>();

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/mouse-link-capture-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString disableMode =
        directory.filePath(QStringLiteral("disable-mode"));
    const QString reenableMode =
        directory.filePath(QStringLiteral("reenable-mode"));

    const QByteArray uri = QByteArrayLiteral("https://example.test/captured");
    QByteArray output = QByteArrayLiteral("\033[?1003h\033]8;;");
    output += uri;
    output += QByteArrayLiteral("\033\\L\033]8;;\033\\");

    LaunchOptions options;
    options.workingDirectory = directory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty -echo; printf '%s' \"$1\"; "
            "while [ ! -e \"$2\" ]; do sleep 0.01; done; "
            "printf '\\033[?1003l'; "
            "while [ ! -e \"$3\" ]; do sleep 0.01; done; "
            "printf '\\033[?1003h'; "
            "exec cat >/dev/null"),
        QStringLiteral("mouse-link-capture-test"),
        QString::fromUtf8(output),
        disableMode,
        reenableMode,
    };
    options.hold = true;
    options.mouseReporting = false;
    options.fontFamily =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();

    QFont font(options.fontFamily);
    font.setPointSizeF(options.fontSize);
    font.setFixedPitch(true);
    font.setStyleHint(QFont::Monospace);
    const QFontMetricsF metrics(font);
    const qreal cellWidth = std::max(
        1.0, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max(1.0, std::ceil(metrics.height()));
    const QPointF linkPosition(cellWidth * 0.5, cellHeight * 0.5);

    TerminalPane pane(options);
    pane.setSize(QSizeF(cellWidth * 8.0, cellHeight * 3.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy hyperlinkQueries(
        controller, &TerminalController::hyperlinkQueryRequested);
    QSignalSpy hyperlinkResolved(
        controller, &TerminalController::hyperlinkResolved);
    QSignalSpy mouseRequests(controller, &TerminalController::mouseRequested);
    QSignalSpy activationResolved(
        controller, &TerminalController::hyperlinkActivationResolved);

    int openCount = 0;
    pane.setUrlOpener([&](const QUrl &opened) {
        ++openCount;
        return opened == QUrl::fromEncoded(uri, QUrl::StrictMode);
    });

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("L")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->terminalMouseTracking(), 1000);
    QVERIFY(!controller->mouseTracking());

    const auto sendHover = [&](Qt::KeyboardModifiers modifiers) {
        QHoverEvent event(QEvent::HoverMove, linkPosition, linkPosition,
                          linkPosition, modifiers);
        QCoreApplication::sendEvent(&pane, &event);
    };
    const auto sendMouse = [&](QEvent::Type type, Qt::MouseButton button,
                               Qt::MouseButtons buttons,
                               Qt::KeyboardModifiers modifiers) {
        QMouseEvent event(type, linkPosition, linkPosition, linkPosition,
                          button, buttons, modifiers);
        QCoreApplication::sendEvent(&pane, &event);
    };
    const auto touch = [](const QString &path) {
        QFile marker(path);
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
    };

    // Link capture follows the raw DEC mode, not the user policy. With raw
    // tracking on and reporting disabled, Ctrl alone remains captured while
    // Ctrl+Shift can still query and activate terminal links.
    QKeyEvent controlPress(QEvent::KeyPress, Qt::Key_Control,
                           Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &controlPress);
    sendHover(Qt::NoModifier);
    QCOMPARE(hyperlinkQueries.count(), 0);
    QCOMPARE(mouseRequests.count(), 0);

    // Raw DEC transitions must recompute hover even though the effective
    // reporting property remains false under the disabled user policy.
    touch(disableMode);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->terminalMouseTracking(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!hyperlinkQueries.isEmpty(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!hyperlinkResolved.isEmpty(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        pane.cursor().shape(), Qt::PointingHandCursor, 1000);

    touch(reenableMode);
    QTRY_VERIFY_WITH_TIMEOUT(controller->terminalMouseTracking(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(pane.cursor().shape(), Qt::ArrowCursor, 1000);

    const qsizetype queriesBeforeShift = hyperlinkQueries.size();
    QKeyEvent shiftPress(QEvent::KeyPress, Qt::Key_Shift,
                         Qt::ControlModifier | Qt::ShiftModifier);
    QCoreApplication::sendEvent(&pane, &shiftPress);
    sendHover(Qt::NoModifier);
    QTRY_VERIFY_WITH_TIMEOUT(
        hyperlinkQueries.size() > queriesBeforeShift, 1000);
    QCOMPARE(mouseRequests.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!hyperlinkResolved.isEmpty(), 1000);

    QKeyEvent shiftRelease(QEvent::KeyRelease, Qt::Key_Shift,
                           Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &shiftRelease);
    QKeyEvent controlRelease(QEvent::KeyRelease, Qt::Key_Control,
                             Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &controlRelease);
    hyperlinkQueries.clear();
    hyperlinkResolved.clear();
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(controller->mouseTracking());

    QCoreApplication::sendEvent(&pane, &controlPress);
    sendHover(Qt::NoModifier);
    QCOMPARE(hyperlinkQueries.count(), 0);
    QCOMPARE(mouseRequests.count(), 1);
    const TerminalMouseInput capturedMotion =
        qvariant_cast<TerminalMouseInput>(
            mouseRequests.constFirst().constFirst());
    QCOMPARE(capturedMotion.action, TerminalMouseInput::Motion);
    QVERIFY(capturedMotion.modifiers & Qt::ControlModifier);

    // The pointer event deliberately omits its modifiers. Stored key state
    // must make Ctrl+Shift both release application capture and satisfy the
    // exact Ctrl-only hyperlink binding after Shift is stripped. With no
    // button held, upstream still reports the hover motion.
    QCoreApplication::sendEvent(&pane, &shiftPress);
    sendHover(Qt::NoModifier);
    QCOMPARE(hyperlinkQueries.count(), 1);
    QCOMPARE(mouseRequests.count(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!hyperlinkResolved.isEmpty(), 1000);
    QCOMPARE(qvariant_cast<TerminalLinkKind>(
                 hyperlinkResolved.constLast().at(2)),
             TerminalLinkKind::Osc8);
    QCOMPARE(hyperlinkResolved.constLast().at(3).toByteArray(), uri);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("copy_url_to_clipboard")));
    QCOMPARE(QGuiApplication::clipboard()->mimeData()->data(
                 QStringLiteral("text/plain")), uri);

    sendMouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
              Qt::NoModifier);
    sendMouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton,
              Qt::NoModifier);
    QTRY_COMPARE_WITH_TIMEOUT(activationResolved.count(), 1, 1000);
    QCOMPARE(openCount, 1);
    QCOMPARE(mouseRequests.count(), 2);

    QCoreApplication::sendEvent(&pane, &shiftRelease);
    QCoreApplication::sendEvent(&pane, &controlRelease);

    // Capture is evaluated for every event. Shift after a captured press
    // suppresses the held motion and release.
    const int beforeCapturedGesture = mouseRequests.count();
    sendMouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
              Qt::NoModifier);
    sendMouse(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton,
              Qt::ShiftModifier);
    sendMouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton,
              Qt::ShiftModifier);
    QCOMPARE(mouseRequests.count(), beforeCapturedGesture + 1);
    QCOMPARE(qvariant_cast<TerminalMouseInput>(
                 mouseRequests.at(beforeCapturedGesture).constFirst()).action,
             TerminalMouseInput::Press);

    // Releasing Shift after a local press allows the later motion and release
    // to use the newly effective application route.
    const int beforeLocalGesture = mouseRequests.count();
    sendMouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
              Qt::ShiftModifier);
    sendMouse(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton,
              Qt::NoModifier);
    sendMouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton,
              Qt::NoModifier);
    QCOMPARE(mouseRequests.count(), beforeLocalGesture + 2);
    QCOMPARE(qvariant_cast<TerminalMouseInput>(
                 mouseRequests.at(beforeLocalGesture).constFirst()).action,
             TerminalMouseInput::Motion);
    QCOMPARE(qvariant_cast<TerminalMouseInput>(
                 mouseRequests.at(beforeLocalGesture + 1).constFirst()).action,
             TerminalMouseInput::Release);

    // Physical button state is independent from each event's route. A local
    // Shift-left followed by a reported right press makes motion identify the
    // first still-held physical button, then reports both releases.
    const int beforeMixedGesture = mouseRequests.count();
    sendMouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
              Qt::ShiftModifier);
    sendMouse(QEvent::MouseButtonPress, Qt::RightButton,
              Qt::LeftButton | Qt::RightButton, Qt::NoModifier);
    sendMouse(QEvent::MouseMove, Qt::NoButton,
              Qt::LeftButton | Qt::RightButton, Qt::NoModifier);
    sendMouse(QEvent::MouseButtonRelease, Qt::RightButton, Qt::LeftButton,
              Qt::NoModifier);
    sendMouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton,
              Qt::NoModifier);
    QCOMPARE(mouseRequests.count(), beforeMixedGesture + 4);
    const TerminalMouseInput mixedPress = qvariant_cast<TerminalMouseInput>(
        mouseRequests.at(beforeMixedGesture).constFirst());
    const TerminalMouseInput mixedMotion = qvariant_cast<TerminalMouseInput>(
        mouseRequests.at(beforeMixedGesture + 1).constFirst());
    const TerminalMouseInput rightRelease = qvariant_cast<TerminalMouseInput>(
        mouseRequests.at(beforeMixedGesture + 2).constFirst());
    const TerminalMouseInput leftRelease = qvariant_cast<TerminalMouseInput>(
        mouseRequests.at(beforeMixedGesture + 3).constFirst());
    QCOMPARE(mixedPress.action, TerminalMouseInput::Press);
    QCOMPARE(mixedPress.button, 2);
    QVERIFY(mixedPress.anyButtonPressed);
    QCOMPARE(mixedMotion.action, TerminalMouseInput::Motion);
    QCOMPARE(mixedMotion.button, 1);
    QVERIFY(mixedMotion.anyButtonPressed);
    QCOMPARE(rightRelease.action, TerminalMouseInput::Release);
    QCOMPARE(rightRelease.button, 2);
    QVERIFY(rightRelease.anyButtonPressed);
    QCOMPARE(leftRelease.action, TerminalMouseInput::Release);
    QCOMPARE(leftRelease.button, 1);
    QVERIFY(!leftRelease.anyButtonPressed);

    // Focus loss does not lock a route: a Shift release remains local.
    const int beforeFocusLoss = mouseRequests.count();
    sendMouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
              Qt::NoModifier);
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(&pane, &focusOut);
    sendMouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton,
              Qt::ShiftModifier);
    QCOMPARE(mouseRequests.count(), beforeFocusLoss + 1);
    QCOMPARE(qvariant_cast<TerminalMouseInput>(
                 mouseRequests.constLast().constFirst()).action,
             TerminalMouseInput::Press);
}

void TerminalPaneTest::resetPreservesSurfaceTitleAndClearsWorkingDirectory()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "printf '\\033]0;metadata-title\\007"
            "\\033]7;file://localhost/\\007metadata-ready\\n'; sleep 5"),
    };
    options.hold = true;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("metadata-ready")), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(pane.title(), QStringLiteral("metadata-title"),
                              1000);
    QTRY_COMPARE_WITH_TIMEOUT(pane.currentDirectory(), QStringLiteral("/"),
                              1000);

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("reset")));
    QTRY_VERIFY_WITH_TIMEOUT(pane.currentDirectory().isEmpty(), 1000);
    QCOMPARE(pane.title(), QStringLiteral("metadata-title"));
    QCOMPARE(pane.splitLaunchOptions(options).workingDirectory,
             QDir::tempPath());

    // OSC and set_surface_title converge on the same apprt base-title layer.
    // Reset publishes no title update, so either writer survives, including
    // an explicitly present empty action value.
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("set_surface_title:")));
    QVERIFY(controller->hasTitle());
    QVERIFY(pane.title().isEmpty());
    const int updatesBeforeActionReset = updates.count();
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("reset")));
    QTRY_VERIFY_WITH_TIMEOUT(updates.count() > updatesBeforeActionReset, 1000);
    QVERIFY(controller->hasTitle());
    QVERIFY(pane.title().isEmpty());
}

void TerminalPaneTest::routesStructuredSequencesAndCancelsThemOnReload()
{
    qRegisterMetaType<TerminalSequenceResolution>();

    const auto unicode = [](quint32 codepoint, quint8 modifiers = 0) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = modifiers,
        };
    };
    const auto sequence = [&](quint32 leaf, QString action,
                              GhosttyKeybindFlags flags = {}) {
        return GhosttyKeybindDefinition{
            .sequence = {unicode('x', GhosttyKeybindCtrl), unicode(leaf)},
            .actions = {std::move(action)},
            .flags = flags,
        };
    };

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindConfig.root = {
        sequence('n', QStringLiteral("new_tab")),
        sequence('u', QStringLiteral("reload_config"),
                 GhosttyKeybindFlags{.consumed = false}),
        sequence('p', QStringLiteral("goto_split:left"),
                 GhosttyKeybindFlags{.performable = true}),
        sequence('e', QStringLiteral("end_key_sequence")),
        sequence('f', QStringLiteral("end_key_sequence"),
                 GhosttyKeybindFlags{.consumed = false}),
    };

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy staged(controller,
                      &TerminalController::sequenceKeyStagingRequested);
    QSignalSpy resolved(controller,
                        &TerminalController::sequenceResolutionRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);
    QSignalSpy applicationActions(
        &pane, &TerminalPane::applicationActionRequested);

    const auto press = [&pane](int key, Qt::KeyboardModifiers modifiers,
                               QString text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };
    const auto release = [&pane](int key, Qt::KeyboardModifiers modifiers,
                                 QString text) {
        QKeyEvent event(QEvent::KeyRelease, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };
    const auto leader = [&] {
        press(Qt::Key_X, Qt::ControlModifier, QString(QChar(0x18)));
    };
    const auto resolution = [&]() {
        return qvariant_cast<TerminalSequenceResolution>(
            resolved.constLast().at(1));
    };

    // Leaders hold only their press. Their release remains visible to the
    // terminal, while a consumed leaf suppresses both its press and release.
    leader();
    QCOMPARE(staged.count(), 1);
    QCOMPARE(forwarded.count(), 0);
    release(Qt::Key_X, Qt::ControlModifier, QString(QChar(0x18)));
    QCOMPARE(forwarded.count(), 1);
    press(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(newTab.count(), 1);
    QCOMPARE(resolution(), TerminalSequenceResolution::Drop);
    release(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(forwarded.count(), 1);

    // An invalid continuation atomically flushes the encoded prefix and the
    // current press; it must not also travel through the ordinary key signal.
    leader();
    press(Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"));
    QCOMPARE(resolution(),
             TerminalSequenceResolution::FlushAndSendCurrent);
    QCOMPARE(forwarded.count(), 1);

    // Unconsumed leaves run their action and replay the entire sequence.
    leader();
    press(Qt::Key_U, Qt::NoModifier, QStringLiteral("u"));
    QCOMPARE(applicationActions.count(), 1);
    QCOMPARE(qvariant_cast<ApplicationAction>(
                 applicationActions.constFirst().constFirst()),
             ApplicationAction::ReloadConfig);
    QCOMPARE(resolution(),
             TerminalSequenceResolution::FlushAndSendCurrent);

    // A performable action that the workspace rejects behaves as absent.
    pane.setWorkspaceActionHandler([](WorkspaceActionRequest request) {
        return request.action != WorkspaceAction::NavigatePane;
    });
    leader();
    press(Qt::Key_P, Qt::NoModifier, QStringLiteral("p"));
    QCOMPARE(resolution(),
             TerminalSequenceResolution::FlushAndSendCurrent);

    // end_key_sequence flushes leaders but consumes the terminating key.
    leader();
    press(Qt::Key_E, Qt::NoModifier, QStringLiteral("e"));
    QCOMPARE(resolution(), TerminalSequenceResolution::Flush);

    // The action itself still flushes only the leaders when the binding is
    // unconsumed, but ordinary handling then encodes its terminating key.
    const int beforeUnconsumedEnd = forwarded.count();
    leader();
    press(Qt::Key_F, Qt::NoModifier, QStringLiteral("f"));
    QCOMPARE(resolution(), TerminalSequenceResolution::Flush);
    QCOMPARE(forwarded.count(), beforeUnconsumedEnd + 1);

    // Live reload is transactional at pane level: pending bytes are dropped
    // before the replacement trie becomes visible.
    leader();
    const int resolutionsBeforeReload = resolved.count();
    LaunchOptions reloaded = options;
    reloaded.keybindConfig.root = {
        GhosttyKeybindDefinition{
            .sequence = {unicode('q', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("new_tab")},
        },
    };
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(resolved.count(), resolutionsBeforeReload + 1);
    QCOMPARE(resolution(), TerminalSequenceResolution::Drop);
    const int beforeFormerLeaf = forwarded.count();
    press(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(forwarded.count(), beforeFormerLeaf + 1);
}

void TerminalPaneTest::routesNamedKeyTablesAndClearsThemOnReload()
{
    const auto unicode = [](quint32 codepoint, quint8 modifiers = 0) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = modifiers,
        };
    };
    const auto binding = [&](QVector<GhosttyKeybindTrigger> sequence,
                             QString action,
                             GhosttyKeybindFlags flags = {}) {
        return GhosttyKeybindDefinition{
            .sequence = std::move(sequence),
            .actions = {std::move(action)},
            .flags = flags,
        };
    };
    const GhosttyKeybindTrigger escape{
        .kind = GhosttyKeybindKeyKind::Physical,
        .physicalName = QStringLiteral("escape"),
    };

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindConfig.root = {
        binding({unicode('m', GhosttyKeybindCtrl)},
                QStringLiteral(R"(activate_key_table:edit\xc3\xa9)"),
                GhosttyKeybindFlags{.performable = true}),
        binding({unicode('o', GhosttyKeybindCtrl)},
                QStringLiteral("activate_key_table_once:once")),
        binding({unicode('u', GhosttyKeybindCtrl)},
                QStringLiteral("activate_key_table:missing"),
                GhosttyKeybindFlags{.performable = true}),
        binding({unicode('d', GhosttyKeybindCtrl)},
                QStringLiteral("deactivate_key_table"),
                GhosttyKeybindFlags{.performable = true}),
    };
    options.keybindConfig.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("edité"),
            .bindings = {
                binding({unicode('n')}, QStringLiteral("new_tab")),
                binding({unicode('x'), unicode('e')},
                        QStringLiteral("end_key_sequence")),
                binding({escape},
                        QStringLiteral("deactivate_key_table")),
            },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("once"),
            .bindings = {
                binding({unicode('x'), unicode('y')},
                        QStringLiteral("new_tab")),
            },
        },
    };

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);
    QSignalSpy tableChanges(&pane, &TerminalPane::activeKeyTablesChanged);
    QSignalSpy resolved(controller,
                        &TerminalController::sequenceResolutionRequested);

    const auto press = [&pane](int key, Qt::KeyboardModifiers modifiers,
                               QString text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };

    // State-dependent no-ops report false at the public broad-action boundary
    // and emit no table notification.
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("activate_key_table:missing")));
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("activate_key_table:")));
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("deactivate_key_table")));
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("deactivate_all_key_tables")));
    QCOMPARE(tableChanges.count(), 0);

    // The same unavailable operations make performable bindings act absent.
    const int beforeUnavailable = forwarded.count();
    press(Qt::Key_U, Qt::ControlModifier, QString(QChar(0x15)));
    press(Qt::Key_D, Qt::ControlModifier, QString(QChar(0x04)));
    QCOMPARE(forwarded.count(), beforeUnavailable + 2);
    QVERIFY(pane.activeKeyTables().isEmpty());
    QCOMPARE(tableChanges.count(), 0);

    const int beforeOrdinary = forwarded.count();
    press(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(forwarded.count(), beforeOrdinary + 1);

    press(Qt::Key_M, Qt::ControlModifier, QString(QChar(0x0d)));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edité")}));
    QCOMPARE(tableChanges.count(), 1);
    press(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(newTab.count(), 1);

    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCOMPARE(resolved.count(), 0);
    press(Qt::Key_E, Qt::NoModifier, QStringLiteral("e"));
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 resolved.constLast().at(1)),
             TerminalSequenceResolution::Flush);
    // With no active sequence, the exact void action remains a successful
    // safe no-op and emits no synthetic worker resolution.
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("end_key_sequence")));
    QCOMPARE(resolved.count(), 1);

    // A performable activation of the already-innermost table behaves as an
    // absent binding and reaches the terminal.
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral(R"(activate_key_table:edit\xc3\xa9)")));
    QCOMPARE(tableChanges.count(), 1);
    const int beforeDuplicate = forwarded.count();
    press(Qt::Key_M, Qt::ControlModifier, QString(QChar(0x0d)));
    QCOMPARE(forwarded.count(), beforeDuplicate + 1);
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edité")}));
    QCOMPARE(tableChanges.count(), 1);

    // Typed single/all deactivation preserves exact stack and notification
    // semantics even when called through the public broad-action boundary.
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("activate_key_table_once:once")));
    QCOMPARE(pane.activeKeyTables(),
             QStringList({QStringLiteral("edité"), QStringLiteral("once")}));
    QCOMPARE(tableChanges.count(), 2);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("deactivate_key_table")));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edité")}));
    QCOMPARE(tableChanges.count(), 3);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("activate_key_table_once:once")));
    QCOMPARE(tableChanges.count(), 4);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("deactivate_all_key_tables")));
    QVERIFY(pane.activeKeyTables().isEmpty());
    QCOMPARE(tableChanges.count(), 5);
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("deactivate_key_table")));
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("deactivate_all_key_tables")));
    QCOMPARE(tableChanges.count(), 5);

    press(Qt::Key_M, Qt::ControlModifier, QString(QChar(0x0d)));
    QCOMPARE(tableChanges.count(), 6);
    press(Qt::Key_Escape, Qt::NoModifier, QString{});
    QVERIFY(pane.activeKeyTables().isEmpty());
    QCOMPARE(tableChanges.count(), 7);

    // A one-shot table pops on the sequence leader, while the retained child
    // trie still resolves the continuation.
    press(Qt::Key_O, Qt::ControlModifier, QString(QChar(0x0f)));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("once")}));
    QCOMPARE(tableChanges.count(), 8);
    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QVERIFY(pane.activeKeyTables().isEmpty());
    QCOMPARE(tableChanges.count(), 9);
    press(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    QCOMPARE(newTab.count(), 2);

    press(Qt::Key_M, Qt::ControlModifier, QString(QChar(0x0d)));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edité")}));
    QCOMPARE(tableChanges.count(), 10);
    LaunchOptions reloaded = options;
    reloaded.keybindConfig.tables.clear();
    pane.applyRuntimeOptions(reloaded);
    QVERIFY(pane.activeKeyTables().isEmpty());
    QCOMPARE(tableChanges.count(), 11);
}

void TerminalPaneTest::replaysInvalidStructuredSequenceThroughPty()
{
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "stty raw -echo; "
            "printf 'keybind-ready'; "
            "payload=$(dd bs=1 count=2 2>/dev/null); "
            "stty sane; "
            "printf 'keybind-bytes:'; "
            "printf '%s' \"$payload\" | od -An -tx1 | tr -d ' \\n'; "
            "printf '\\n'")};
    options.hold = true;
    options.keybindingsConfigured = true;
    options.keybindConfig.root = {GhosttyKeybindDefinition{
        .sequence = {
            GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::Unicode,
                .unicodeCodepoint = 'x',
                .modifiers = GhosttyKeybindCtrl,
            },
            GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::Unicode,
                .unicodeCodepoint = 'n',
            },
        },
        .actions = {QStringLiteral("new_tab")},
    }};

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy errors(controller, &TerminalController::errorOccurred);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("keybind-ready")), 5000);

    const auto send = [&pane](int key, Qt::KeyboardModifiers modifiers,
                              const QString &text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
        QCoreApplication::sendEvent(&pane, &event);
    };

    // This valid sequence is consumed and must contribute no PTY bytes.
    send(Qt::Key_X, Qt::ControlModifier, QString(QChar(0x18)));
    send(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(newTab.count(), 1);

    // The same leader followed by an invalid key replays byte-exact input in
    // order: Ctrl-X (0x18), then z (0x7a).
    send(Qt::Key_X, Qt::ControlModifier, QString(QChar(0x18)));
    send(Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"));
    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("keybind-bytes:187a")), 5000);
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty()
                 ? ""
                 : qPrintable(errors.constFirst().constFirst().toString()));
}

void TerminalPaneTest::rejectsMalformedFrontendActionsWithoutSideEffects()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy copied(controller, &TerminalController::copyRequested);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);
    QSignalSpy applicationActions(
        &pane, &TerminalPane::applicationActionRequested);

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    clipboard->setText(QStringLiteral("malformed-action-sentinel"));
    const qreal originalFontSize = pane.fontPointSize();

    const QStringList malformed{
        QStringLiteral("copy_to_clipboard:"),
        QStringLiteral("copy_to_clipboard:bogus"),
        QStringLiteral("copy_title_to_clipboard:"),
        QStringLiteral("copy_title_to_clipboard:bogus"),
        QStringLiteral("paste_from_clipboard:"),
        QStringLiteral("paste_from_clipboard:bogus"),
        QStringLiteral("reset_font_size:bogus"),
        QStringLiteral("reset_font_size:"),
        QStringLiteral("reload_config:bogus"),
        QStringLiteral("close_window:bogus"),
        QStringLiteral("ignore:bogus"),
        QStringLiteral("increase_font_size"),
        QStringLiteral("increase_font_size:"),
        QStringLiteral("decrease_font_size"),
        QStringLiteral("decrease_font_size:"),
        QStringLiteral("set_font_size"),
        QStringLiteral("set_font_size:"),
    };
    for (const QString &serialized : malformed) {
        QVERIFY2(!pane.executeConfiguredAction(serialized),
                 qPrintable(serialized));
    }

    QCOMPARE(copied.count(), 0);
    QCOMPARE(pasted.count(), 0);
    QCOMPARE(applicationActions.count(), 0);
    QCOMPARE(pane.fontPointSize(), originalFontSize);
    clipboard->clear();
}

QTEST_MAIN(TerminalPaneTest)

#include "test_terminal_pane.moc"
