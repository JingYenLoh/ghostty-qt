#include "application_controller.h"
#include "terminal_cell_metrics.h"
#include "terminal_controller.h"
#include "terminal_geometry.h"
#include "terminal_pane.h"
#include "terminal_pane_render_probe_p.h"
#include "terminal_workspace.h"

#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace {

Qt::WindowStates presentationWindowStates(Qt::WindowStates states)
{
    return states
        & Qt::WindowStates(Qt::WindowMinimized
                           | Qt::WindowMaximized
                           | Qt::WindowFullScreen);
}

struct WindowFactoryHarness {
    struct Presentation {
        Qt::WindowStates states;
        int visibilityBeforeFullscreen = QWindow::Windowed;
        QSize size;
        QSize minimumSize;
    };

    int calls = 0;
    int failOnCall = 0;
    qreal chromeWidth = 0.0;
    qreal chromeHeight = 0.0;
    QSize simulatedCompositorSize;
    bool createTabAtPresentation = false;
    QVector<Presentation> presentations;
    QVector<bool> sessionStartedAtPresentation;

    ApplicationController::WindowFactory factory()
    {
        return [this]() -> std::expected<ApplicationWindow, QString> {
            ++calls;
            if (failOnCall == calls) {
                return std::unexpected(
                    QStringLiteral("injected window creation failure"));
            }

            auto *window = new QQuickWindow;
            window->setWidth(800);
            window->setHeight(500);
            window->setProperty("terminalChromeWidth", chromeWidth);
            window->setProperty("terminalChromeHeight", chromeHeight);
            auto *workspace =
                new TerminalWorkspace(window->contentItem());
            workspace->setParentItem(window->contentItem());
            const auto resizeWorkspace = [this, window, workspace] {
                workspace->setSize(QSizeF(
                    std::max(0.0,
                             static_cast<qreal>(window->width())
                                 - chromeWidth),
                    std::max(0.0,
                             static_cast<qreal>(window->height())
                                 - chromeHeight)));
            };
            resizeWorkspace();
            QObject::connect(window, &QWindow::widthChanged,
                             workspace, resizeWorkspace);
            QObject::connect(window, &QWindow::heightChanged,
                             workspace, resizeWorkspace);
            QObject::connect(window, &QWindow::visibleChanged, window,
                             [this, window, workspace](bool visible) {
                                 if (!visible) return;
                                 TerminalController *const controller =
                                     workspace->findChild<TerminalController *>();
                                 sessionStartedAtPresentation.append(
                                     controller != nullptr
                                     && controller->sessionStarted());
                                 if (simulatedCompositorSize.isValid()) {
                                     const QSize compositorSize =
                                         simulatedCompositorSize;
                                     QObject::connect(
                                         window, &QQuickWindow::frameSwapped,
                                         window,
                                         [window, compositorSize] {
                                             window->resize(compositorSize);
                                         }, Qt::SingleShotConnection);
                                     window->update();
                                 }
                                 if (createTabAtPresentation) {
                                     workspace->newTab();
                                 }
                                 presentations.append({
                                     .states = presentationWindowStates(
                                         window->windowStates()),
                                     .visibilityBeforeFullscreen =
                                         window->property(
                                             "visibilityBeforeFullscreen")
                                             .toInt(),
                                     .size = window->size(),
                                     .minimumSize = window->minimumSize(),
                                 });
                             });
            return ApplicationWindow{window, workspace};
        };
    }
};

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(QByteArray name, QByteArray value)
        : name_(std::move(name))
        , wasSet_(qEnvironmentVariableIsSet(name_.constData()))
        , previousValue_(qgetenv(name_.constData()))
    {
        (void) qputenv(name_.constData(), value);
    }

    ~ScopedEnvironmentVariable()
    {
        if (wasSet_) {
            (void) qputenv(name_.constData(), previousValue_);
        } else {
            (void) qunsetenv(name_.constData());
        }
    }

    Q_DISABLE_COPY_MOVE(ScopedEnvironmentVariable)

private:
    QByteArray name_;
    bool wasSet_ = false;
    QByteArray previousValue_;
};

LaunchOptions baseOptions(const QString &directory)
{
    LaunchOptions options;
    options.workingDirectory = directory;
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.quitAfterLastWindowClosed = false;
    options.keybindingsConfigured = true;
    return options;
}

TerminalPane *onlyPane(TerminalWorkspace *workspace)
{
    if (workspace == nullptr) return nullptr;
    const QList<TerminalPane *> panes =
        workspace->findChildren<TerminalPane *>();
    return panes.size() == 1 ? panes.constFirst() : nullptr;
}

TerminalController *onlyController(TerminalWorkspace *workspace)
{
    TerminalPane *const pane = onlyPane(workspace);
    return pane != nullptr
        ? pane->findChild<TerminalController *>() : nullptr;
}

PaneId activePaneId(TerminalWorkspace *workspace)
{
    if (workspace == nullptr || workspace->tabCount() != 1) return {};
    const TabListEntry *const entry = workspace->tabModel()->entryAt(0);
    return entry != nullptr ? entry->activePaneId : PaneId{};
}

void closeWorkspace(TerminalWorkspace *workspace)
{
    QVERIFY(workspace != nullptr);
    workspace->requestWindowClose();
}

QSize gridWindowSize(const TerminalCellMetrics &metrics,
                     quint32 columns, quint32 rows,
                     qreal chromeWidth, qreal chromeHeight)
{
    const auto extent = [](qreal cellExtent, quint32 cells, qreal chrome) {
        const long double pixels =
            static_cast<long double>(cellExtent)
                * static_cast<long double>(cells)
            + static_cast<long double>(chrome);
        constexpr int maximum = std::numeric_limits<int>::max();
        if (!std::isfinite(pixels)
            || pixels >= static_cast<long double>(maximum)) {
            return maximum;
        }
        return std::max(1, static_cast<int>(std::ceil(pixels)));
    };
    return {
        extent(metrics.cellWidth, columns, chromeWidth),
        extent(metrics.cellHeight, rows, chromeHeight),
    };
}

} // namespace

class ApplicationControllerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void configuresInitialWindowStateBeforePresentation_data();
    void configuresInitialWindowStateBeforePresentation();
    void defersNonWindowedSessionUntilExposedGeometry_data();
    void defersNonWindowedSessionUntilExposedGeometry();
    void windowStateReloadAffectsOnlyFutureWindows();
    void configuresInitialWindowGeometryBeforePresentation_data();
    void configuresInitialWindowGeometryBeforePresentation();
    void windowGeometryReloadAffectsOnlyFutureWindows();
    void initialGeometryDestructionCannotLeaveHalfRegisteredWindow_data();
    void initialGeometryDestructionCannotLeaveHalfRegisteredWindow();
    void preservesCompositeSourceAndWindowInheritancePolicies();
    void residentProcessReloadsRecreatesAndQuitsWithZeroWindows();
    void suppressedStartupPreservesFirstSessionOptions();
    void failedLazyCreationDoesNotConsumeFirstSession();
    void terminalInitializationFailurePromotesNextSession();
    void waitingControllersCancelWithoutCreatingWorkers();
    void immediateTabWinsInitialSessionLease();
    void reverseExposureGrantsInitialSessionToFirstStarter();
    void ordinaryInheritanceDoesNotWaitForFirstSession();
    void deferredRequestsRemainOrderedAcrossStart();
    void ordinaryCloseUsesOnlyTheFinalWindowForLifetimePolicy();
    void successfulReplacementCancelsDelayedQuit();
    void sourceLessActivationMatchesUpstreamInheritance();
    void sourceLessActivationCancelsQuitAndReportsFailure();
    void sourceLessActivationRequiresStartupDecision();
    void sourceLessActivationProjectsDesktopContext();
    void reentrantWindowCreationIsRejected();
    void showDestructionCannotLeaveHalfRegisteredWindow();
    void failedInitialPresentationPreservesFirstSession();
    void explicitQuitAggregatesEveryWindowIntoOneConfirmation();
    void pendingQuitRehostsAfterItsWindowDisappears();
    void failedReplacementLeavesDelayedQuitArmed();
    void rejectsWorkspaceOutsideWindowOwnership();
    void creationObserversCannotLeaveHalfRegisteredWindow_data();
    void creationObserversCannotLeaveHalfRegisteredWindow();
    void creationObserverCannotDetachWorkspace();
    void workspaceLossRetiresOwningWindow();
};

void ApplicationControllerTest::initTestCase()
{
    QVERIFY(QDir().mkpath(
        QDir::current().filePath(QStringLiteral("tmp"))));
}

void ApplicationControllerTest::configuresInitialWindowStateBeforePresentation_data()
{
    QTest::addColumn<bool>("maximize");
    QTest::addColumn<bool>("fullscreen");
    QTest::addColumn<int>("expectedState");
    QTest::addColumn<int>("expectedVisibility");
    QTest::addColumn<int>("expectedRestoreVisibility");

    QTest::newRow("windowed")
        << false << false
        << static_cast<int>(Qt::WindowNoState)
        << static_cast<int>(QWindow::Windowed)
        << static_cast<int>(QWindow::Windowed);
    QTest::newRow("maximized")
        << true << false
        << static_cast<int>(Qt::WindowMaximized)
        << static_cast<int>(QWindow::Maximized)
        << static_cast<int>(QWindow::Maximized);
    QTest::newRow("fullscreen")
        << false << true
        << static_cast<int>(Qt::WindowFullScreen)
        << static_cast<int>(QWindow::FullScreen)
        << static_cast<int>(QWindow::Windowed);
    QTest::newRow("fullscreen-restores-maximized")
        << true << true
        << static_cast<int>(Qt::WindowFullScreen)
        << static_cast<int>(QWindow::FullScreen)
        << static_cast<int>(QWindow::Maximized);
}

void ApplicationControllerTest::configuresInitialWindowStateBeforePresentation()
{
    QFETCH(bool, maximize);
    QFETCH(bool, fullscreen);
    QFETCH(int, expectedState);
    QFETCH(int, expectedVisibility);
    QFETCH(int, expectedRestoreVisibility);

    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.maximize = maximize;
    options.fullscreen = fullscreen;
    options.resizeOverlay.mode = ResizeOverlayMode::Always;
    ApplicationController controller(options, harness.factory(), false);

    const auto created = controller.createInitialWindow();
    QVERIFY(created.has_value());
    QCOMPARE(harness.presentations.size(), 1);
    QCOMPARE(harness.sessionStartedAtPresentation.size(), 1);
    QCOMPARE(harness.sessionStartedAtPresentation.constFirst(),
             !maximize && !fullscreen);
    const WindowFactoryHarness::Presentation &presentation =
        harness.presentations.constFirst();
    QCOMPARE(static_cast<int>(presentation.states), expectedState);
    QCOMPARE(presentation.visibilityBeforeFullscreen,
             expectedRestoreVisibility);
    QCOMPARE(static_cast<int>(created->window->visibility()),
             expectedVisibility);
}

void ApplicationControllerTest::defersNonWindowedSessionUntilExposedGeometry_data()
{
    QTest::addColumn<bool>("maximize");
    QTest::addColumn<bool>("fullscreen");

    QTest::newRow("maximized") << true << false;
    QTest::newRow("fullscreen") << false << true;
    QTest::newRow("fullscreen-restores-maximized") << true << true;
}

void ApplicationControllerTest::defersNonWindowedSessionUntilExposedGeometry()
{
    QFETCH(bool, maximize);
    QFETCH(bool, fullscreen);

    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/deferred-geometry-XXXXXX")));
    QVERIFY(temporary.isValid());
    const QString resultPath = temporary.filePath(QStringLiteral("winsize"));

    WindowFactoryHarness harness;
    harness.simulatedCompositorSize = QSize(913, 617);
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.windowWidth = 40;
    options.windowHeight = 12;
    options.maximize = maximize;
    options.fullscreen = fullscreen;
    options.program = {
        QStringLiteral(GHOSTTY_QT_TEST_PTY_GEOMETRY_PROBE),
        resultPath,
    };

    ApplicationController application(options, harness.factory(), false);
    const auto created = application.createInitialWindow();
    QVERIFY(created.has_value());
    QCOMPARE(harness.sessionStartedAtPresentation.size(), 1);
    QVERIFY(!harness.sessionStartedAtPresentation.constFirst());

    TerminalPane *initialPane = nullptr;
    for (TerminalPane *const pane :
         created->workspace->findChildren<TerminalPane *>()) {
        if (terminalPaneRenderProbe(pane).initialGeometry.has_value()) {
            initialPane = pane;
            break;
        }
    }
    QVERIFY(initialPane != nullptr);
    TerminalController *const controller =
        initialPane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QVERIFY(!controller->sessionStarted());
    QVERIFY(!controller->running());
    QVERIFY(!controller->activeProcess());
    QVERIFY(controller->findChild<QThread *>() == nullptr);
    QVERIFY(!initialPane->resizeOverlayVisible());
    QSignalSpy runningChanged(controller, &TerminalController::runningChanged);

    QTRY_VERIFY_WITH_TIMEOUT(created->window->isExposed(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->sessionStarted(), 1000);
    QVERIFY(controller->findChild<QThread *>() != nullptr);
    const TerminalCellMetrics metrics = terminalCellMetrics(
        options.fontFamily, options.fontSize);
    const std::optional<TerminalSessionGeometry> expected =
        terminalSessionGeometryForViewport(
            created->workspace->width(), created->workspace->height(),
            metrics.cellWidth, metrics.cellHeight,
            created->window->devicePixelRatio());
    QVERIFY(expected.has_value());
    QVERIFY(controller->launchGeometry() == expected);
    QTest::qWait(25);
    QVERIFY(!initialPane->resizeOverlayVisible());

    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(resultPath), 2000);
    QFile result(resultPath);
    QVERIFY(result.open(QIODevice::ReadOnly));
    const QList<QByteArray> dimensions =
        result.readAll().simplified().split(':');
    QCOMPARE(dimensions.size(), 5);
    QCOMPARE(dimensions.at(0), QByteArrayLiteral("ghostty-qt-pty-geometry"));
    QCOMPARE(dimensions.at(1).toInt(), expected->columns);
    QCOMPARE(dimensions.at(2).toInt(), expected->rows);
    QCOMPARE(dimensions.at(3).toInt(), expected->surfaceWidthPixels);
    QCOMPARE(dimensions.at(4).toInt(), expected->surfaceHeightPixels);

    QEvent duplicateExpose(QEvent::Expose);
    QCoreApplication::sendEvent(created->window, &duplicateExpose);
    created->window->resize(created->window->size() + QSize(1, 1));
    QTest::qWait(25);
    const auto startNotifications = std::ranges::count_if(
        runningChanged, [](const QList<QVariant> &arguments) {
            return arguments.constFirst().toBool();
        });
    QCOMPARE(startNotifications, 1);
    const std::optional<TerminalSessionGeometry> launchGeometry =
        controller->launchGeometry();
    QVERIFY(!controller->startSession(TerminalSessionGeometry{
        .columns = 1,
        .rows = 1,
        .cellWidthPixels = 1,
        .cellHeightPixels = 1,
        .surfaceWidthPixels = 1,
        .surfaceHeightPixels = 1,
    }));
    QVERIFY(controller->launchGeometry() == launchGeometry);
}

void ApplicationControllerTest::windowStateReloadAffectsOnlyFutureWindows()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(options, harness.factory(), false);

    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QCOMPARE(harness.presentations.size(), 1);
    QCOMPARE(presentationWindowStates(initial->window->windowStates()),
             Qt::WindowStates(Qt::WindowNoState));

    LaunchOptions maximized = options;
    maximized.maximize = true;
    controller.applyLaunchOptions(maximized);
    QCOMPARE(presentationWindowStates(initial->window->windowStates()),
             Qt::WindowStates(Qt::WindowNoState));
    QCOMPARE(initial->window->property(
                 "visibilityBeforeFullscreen").toInt(),
             static_cast<int>(QWindow::Windowed));

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.presentations.size(), 2, 1000);
    const ApplicationWindow actionCreated = controller.windows().constLast();
    QCOMPARE(presentationWindowStates(actionCreated.window->windowStates()),
             Qt::WindowStates(Qt::WindowMaximized));
    QCOMPARE(actionCreated.window->visibility(), QWindow::Maximized);

    LaunchOptions fullscreen = maximized;
    fullscreen.maximize = false;
    fullscreen.fullscreen = true;
    controller.applyLaunchOptions(fullscreen);
    QCOMPARE(presentationWindowStates(initial->window->windowStates()),
             Qt::WindowStates(Qt::WindowNoState));
    QCOMPARE(presentationWindowStates(actionCreated.window->windowStates()),
             Qt::WindowStates(Qt::WindowMaximized));

    for (const ApplicationWindow &window : controller.windows()) {
        closeWorkspace(window.workspace);
    }
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.presentations.size(), 3, 1000);
    const ApplicationWindow residentReplacement =
        controller.windows().constFirst();
    QCOMPARE(presentationWindowStates(
                 residentReplacement.window->windowStates()),
             Qt::WindowStates(Qt::WindowFullScreen));
    QCOMPARE(residentReplacement.window->visibility(),
             QWindow::FullScreen);

    closeWorkspace(residentReplacement.workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);

    LaunchOptions both = fullscreen;
    both.maximize = true;
    controller.applyLaunchOptions(both);
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 1);
    QCOMPARE(harness.presentations.size(), 4);
    const ApplicationWindow activated = controller.windows().constFirst();
    QCOMPARE(presentationWindowStates(activated.window->windowStates()),
             Qt::WindowStates(Qt::WindowFullScreen));
    QCOMPARE(activated.window->visibility(), QWindow::FullScreen);
    QCOMPARE(harness.presentations.at(3).visibilityBeforeFullscreen,
             static_cast<int>(QWindow::Maximized));
}

void ApplicationControllerTest::configuresInitialWindowGeometryBeforePresentation_data()
{
    QTest::addColumn<quint32>("columns");
    QTest::addColumn<quint32>("rows");
    QTest::addColumn<double>("fontSize");
    QTest::addColumn<bool>("usesConfiguredSize");

    QTest::newRow("default")
        << quint32(0) << quint32(0) << 12.0 << false;
    QTest::newRow("width-only")
        << quint32(40) << quint32(0) << 12.0 << false;
    QTest::newRow("height-only")
        << quint32(0) << quint32(12) << 12.0 << false;
    QTest::newRow("paired")
        << quint32(40) << quint32(12) << 12.0 << true;
    QTest::newRow("defensive-minimum")
        << quint32(1) << quint32(1) << 12.0 << true;
    QTest::newRow("overflow-safe-screen-clamp")
        << std::numeric_limits<quint32>::max()
        << std::numeric_limits<quint32>::max()
        << 12.0 << true;
    QTest::newRow("screen-bounds-large-font-minimum")
        << quint32(10) << quint32(4) << 1000.0 << true;
}

void ApplicationControllerTest::configuresInitialWindowGeometryBeforePresentation()
{
    QFETCH(quint32, columns);
    QFETCH(quint32, rows);
    QFETCH(double, fontSize);
    QFETCH(bool, usesConfiguredSize);

    WindowFactoryHarness harness;
    harness.chromeWidth = 3.25;
    harness.chromeHeight = 41.5;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.windowWidth = columns;
    options.windowHeight = rows;
    options.fontSize = fontSize;
    ApplicationController controller(options, harness.factory(), false);

    const auto created = controller.createInitialWindow();
    QVERIFY(created.has_value());
    QCOMPARE(harness.presentations.size(), 1);

    const TerminalCellMetrics metrics = terminalCellMetrics(
        options.fontFamily, options.fontSize);
    QSize expectedMinimum = gridWindowSize(
        metrics, 10, 4, harness.chromeWidth, harness.chromeHeight);
    const QSize available = created->window->screen() != nullptr
        ? created->window->screen()->availableGeometry().size()
        : QSize();
    if (available.width() > 0) {
        expectedMinimum.setWidth(
            std::min(expectedMinimum.width(), available.width()));
    }
    if (available.height() > 0) {
        expectedMinimum.setHeight(
            std::min(expectedMinimum.height(), available.height()));
    }
    QCOMPARE(created->window->minimumSize(), expectedMinimum);
    QCOMPARE(harness.presentations.constFirst().minimumSize,
             expectedMinimum);

    QSize expected(800, 500);
    if (usesConfiguredSize) {
        expected = gridWindowSize(
            metrics,
            std::max(columns, quint32(10)),
            std::max(rows, quint32(4)),
            harness.chromeWidth, harness.chromeHeight);
        if (available.width() > 0) {
            expected.setWidth(
                std::min(expected.width(), available.width()));
        }
        if (available.height() > 0) {
            expected.setHeight(
                std::min(expected.height(), available.height()));
        }
        expected = expected.expandedTo(expectedMinimum);
    }
    QCOMPARE(created->window->size(), expected);
    QCOMPARE(harness.presentations.constFirst().size, expected);

    TerminalPane *const pane = onlyPane(created->workspace);
    QVERIFY(pane != nullptr);
    const std::optional<TerminalSessionGeometry> expectedInitialGeometry =
        terminalSessionGeometryForViewport(
            created->workspace->width(), created->workspace->height(),
            metrics.cellWidth, metrics.cellHeight,
            created->window->devicePixelRatio());
    QVERIFY(expectedInitialGeometry.has_value());
    QVERIFY(terminalPaneRenderProbe(pane).initialGeometry
            == expectedInitialGeometry);
}

void ApplicationControllerTest::windowGeometryReloadAffectsOnlyFutureWindows()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.windowWidth = 20;
    options.windowHeight = 8;
    ApplicationController controller(options, harness.factory(), false);

    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    const QSize initialSize = gridWindowSize(
        terminalCellMetrics(options.fontFamily, options.fontSize),
        options.windowWidth, options.windowHeight, 0.0, 0.0);
    QCOMPARE(initial->window->size(), initialSize);
    TerminalPane *const initialPane = onlyPane(initial->workspace);
    QVERIFY(initialPane != nullptr);

    LaunchOptions reloaded = options;
    reloaded.windowWidth = 30;
    reloaded.windowHeight = 10;
    controller.applyLaunchOptions(reloaded);
    QCOMPARE(initial->window->size(), initialSize);

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.presentations.size(), 2, 1000);
    const QSize reloadedSize = gridWindowSize(
        terminalCellMetrics(reloaded.fontFamily, reloaded.fontSize),
        reloaded.windowWidth, reloaded.windowHeight, 0.0, 0.0);
    QCOMPARE(controller.windows().constLast().window->size(), reloadedSize);

    QVERIFY(initialPane->executeConfiguredAction(
        QStringLiteral("set_font_size:18")));
    QVERIFY(initialPane->executeConfiguredAction(
        QStringLiteral("new_window")));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 3, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.presentations.size(), 3, 1000);
    const QSize inheritedFontSize = gridWindowSize(
        terminalCellMetrics(reloaded.fontFamily, 18.0),
        reloaded.windowWidth, reloaded.windowHeight, 0.0, 0.0);
    const ApplicationWindow inherited = controller.windows().constLast();
    QCOMPARE(inherited.workspace->effectiveLaunchOptions().fontSize, 18.0);
    QCOMPARE(inherited.window->size(), inheritedFontSize);

    const QVector<ApplicationWindow> live = controller.windows();
    for (const ApplicationWindow &window : live) {
        closeWorkspace(window.workspace);
    }
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.presentations.size(), 4, 1000);
    QCOMPARE(controller.windows().constFirst().window->size(), reloadedSize);
    closeWorkspace(controller.windows().constFirst().workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);

    LaunchOptions activated = reloaded;
    activated.windowWidth = 36;
    activated.windowHeight = 11;
    controller.applyLaunchOptions(activated);
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 1);
    QCOMPARE(harness.presentations.size(), 5);
    QCOMPARE(
        controller.windows().constFirst().window->size(),
        gridWindowSize(
            terminalCellMetrics(activated.fontFamily, activated.fontSize),
            activated.windowWidth, activated.windowHeight, 0.0, 0.0));
}

void ApplicationControllerTest::initialGeometryDestructionCannotLeaveHalfRegisteredWindow_data()
{
    QTest::addColumn<bool>("invalidateMinimum");
    QTest::newRow("minimum-size") << true;
    QTest::newRow("configured-resize") << false;
}

void ApplicationControllerTest::initialGeometryDestructionCannotLeaveHalfRegisteredWindow()
{
    QFETCH(bool, invalidateMinimum);
    QPointer<QQuickWindow> window;
    QPointer<TerminalWorkspace> workspace;
    ApplicationController::WindowFactory factory = [&]()
        -> std::expected<ApplicationWindow, QString> {
        window = new QQuickWindow;
        window->resize(800, 500);
        workspace = new TerminalWorkspace(window->contentItem());
        workspace->setParentItem(window->contentItem());
        workspace->setSize(QSizeF(window->size()));
        const auto invalidate = [guarded = workspace] {
            delete guarded.data();
        };
        if (invalidateMinimum) {
            QObject::connect(window, &QWindow::minimumWidthChanged,
                             window, invalidate, Qt::SingleShotConnection);
        } else {
            QObject::connect(window, &QWindow::widthChanged,
                             window, invalidate, Qt::SingleShotConnection);
        }
        return ApplicationWindow{window, workspace};
    };

    LaunchOptions options = baseOptions(QDir::currentPath());
    options.windowWidth = 40;
    options.windowHeight = 12;
    ApplicationController controller(options, std::move(factory), false);
    const auto created = controller.createInitialWindow();
    QVERIFY(!created.has_value());
    QVERIFY(created.error().contains(QStringLiteral("initial geometry")));
    QVERIFY(window.isNull());
    QVERIFY(workspace.isNull());
    QCOMPARE(controller.windowCount(), 0);
    QCOMPARE(controller.lifetimeController()->registeredWindowCount(), 0);
}

void ApplicationControllerTest::preservesCompositeSourceAndWindowInheritancePolicies()
{
    QTemporaryDir directories(
        QDir::current().filePath(
            QStringLiteral("tmp/application-controller-inheritance-XXXXXX")));
    QVERIFY(directories.isValid());
    const QString firstDirectory =
        QDir(directories.path()).filePath(QStringLiteral("first"));
    const QString secondDirectory =
        QDir(directories.path()).filePath(QStringLiteral("second"));
    QVERIFY(QDir().mkpath(firstDirectory));
    QVERIFY(QDir().mkpath(secondDirectory));

    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(firstDirectory);
    options.fontSize = 13.0;
    ApplicationController controller(options, harness.factory(), false);

    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QCOMPARE(controller.windowCount(), 1);
    TerminalPane *const firstPane = onlyPane(initial->workspace);
    QVERIFY(firstPane != nullptr);
    QCOMPARE(activePaneId(initial->workspace), PaneId(1));
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_font_size:18")));
    QCOMPARE(firstPane->fontPointSize(), 18.0);

    // Create a second window from application defaults. It also owns PaneId 1,
    // proving that a naked PaneId is not a process-wide identity.
    LaunchOptions fallback = options;
    fallback.workingDirectory = secondDirectory;
    fallback.fontSize = 11.0;
    fallback.windowInheritWorkingDirectory = false;
    fallback.windowInheritFontSize = false;
    controller.applyLaunchOptions(fallback);
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    const QVector<ApplicationWindow> firstTwo = controller.windows();
    TerminalWorkspace *const secondWorkspace = firstTwo.constLast().workspace;
    TerminalPane *const secondPane = onlyPane(secondWorkspace);
    QVERIFY(secondPane != nullptr);
    QCOMPARE(activePaneId(secondWorkspace), PaneId(1));
    QCOMPARE(secondWorkspace->effectiveLaunchOptions().workingDirectory,
             secondDirectory);
    QCOMPARE(secondWorkspace->effectiveLaunchOptions().fontSize, 11.0);
    QVERIFY(secondWorkspace->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!secondWorkspace->effectiveLaunchOptions().hold);

    LaunchOptions inheriting = fallback;
    inheriting.windowInheritWorkingDirectory = true;
    inheriting.windowInheritFontSize = true;
    controller.applyLaunchOptions(inheriting);
    QVERIFY(initial->workspace->effectiveLaunchOptions()
                .windowInheritWorkingDirectory);
    QVERIFY(secondWorkspace->effectiveLaunchOptions()
                .windowInheritWorkingDirectory);

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("new_window")));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 3, 1000);
    TerminalWorkspace *const inheritedFirst =
        controller.windows().constLast().workspace;
    QCOMPARE(inheritedFirst->effectiveLaunchOptions().workingDirectory,
             firstDirectory);
    QCOMPARE(inheritedFirst->effectiveLaunchOptions().fontSize, 18.0);
    QVERIFY(inheritedFirst->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!inheritedFirst->effectiveLaunchOptions().hold);

    QVERIFY(secondPane->executeConfiguredAction(
        QStringLiteral("new_window")));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 4, 1000);
    TerminalWorkspace *const inheritedSecond =
        controller.windows().constLast().workspace;
    QCOMPARE(inheritedSecond->effectiveLaunchOptions().workingDirectory,
             secondDirectory);
    QCOMPARE(inheritedSecond->effectiveLaunchOptions().fontSize, 11.0);

    // Delivery validates the composite source again. If its whole window has
    // disappeared, the most recently active live pane supplies inheritance.
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow,
                                initial->workspace, PaneId(1)));
    QPointer<QQuickWindow> staleSourceWindow(initial->window);
    delete initial->window;
    QVERIFY(staleSourceWindow.isNull());
    QCOMPARE(controller.windowCount(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 4, 1000);
    const TerminalWorkspace *const staleSourceFallback =
        controller.windows().constLast().workspace;
    QCOMPARE(staleSourceFallback->effectiveLaunchOptions().workingDirectory,
             secondDirectory);
    QCOMPARE(staleSourceFallback->effectiveLaunchOptions().fontSize, 11.0);
}

void ApplicationControllerTest::residentProcessReloadsRecreatesAndQuitsWithZeroWindows()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());

    QPointer<QQuickWindow> retiredWindow(initial->window);
    QPointer<TerminalWorkspace> retiredWorkspace(initial->workspace);
    closeWorkspace(initial->workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(retiredWindow.isNull(), 1000);
    QVERIFY(retiredWorkspace.isNull());
    QVERIFY(!controller.lifetimeController()->quitPending());
    QVERIFY(!controller.lifetimeController()->hasRequestedQuit());

    QSignalSpy openConfig(&controller,
                          &ApplicationController::configOpenRequested);
    QVERIFY(controller.dispatch(ApplicationAction::OpenConfig));
    QCOMPARE(openConfig.count(), 1);

    QSignalSpy reload(&controller,
                      &ApplicationController::configReloadRequested);
    QVERIFY(controller.dispatch(ApplicationAction::ReloadConfig));
    QCOMPARE(reload.count(), 1);

    LaunchOptions reloaded = options;
    reloaded.fontSize = 17.0;
    controller.applyLaunchOptions(reloaded);
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    const ApplicationWindow replacement = controller.windows().constFirst();
    QCOMPARE(replacement.workspace->effectiveLaunchOptions().fontSize, 17.0);
    QVERIFY(replacement.workspace->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!replacement.workspace->effectiveLaunchOptions().hold);

    closeWorkspace(replacement.workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QSignalSpy committed(&controller,
                         &ApplicationController::applicationQuitCommitted);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    QVERIFY(controller.dispatch(ApplicationAction::Quit));
    QCOMPARE(committed.count(), 1);
    QCOMPARE(quit.count(), 1);
}

void ApplicationControllerTest::rejectsWorkspaceOutsideWindowOwnership()
{
    QPointer<QQuickWindow> window;
    QPointer<TerminalWorkspace> workspace;
    ApplicationController::WindowFactory factory = [&]()
        -> std::expected<ApplicationWindow, QString> {
        window = new QQuickWindow;
        workspace = new TerminalWorkspace;
        workspace->setParentItem(window->contentItem());
        return ApplicationWindow{window, workspace};
    };

    ApplicationController controller(
        baseOptions(QDir::currentPath()), std::move(factory), false);
    const auto created = controller.createInitialWindow();
    QVERIFY(!created.has_value());
    QVERIFY(created.error().contains(QStringLiteral("QObject ownership")));
    QVERIFY(window.isNull());
    QVERIFY(workspace.isNull());
    QCOMPARE(controller.windowCount(), 0);
    QCOMPARE(controller.lifetimeController()->registeredWindowCount(), 0);
}

void ApplicationControllerTest::creationObserversCannotLeaveHalfRegisteredWindow_data()
{
    QTest::addColumn<bool>("deleteWorkspace");
    QTest::newRow("workspace") << true;
    QTest::newRow("window") << false;
}

void ApplicationControllerTest::creationObserversCannotLeaveHalfRegisteredWindow()
{
    QFETCH(bool, deleteWorkspace);
    WindowFactoryHarness harness;
    ApplicationController controller(
        baseOptions(QDir::currentPath()), harness.factory(), false);
    QVERIFY(controller.startWithoutInitialWindow());

    QPointer<QQuickWindow> observedWindow;
    QPointer<TerminalWorkspace> observedWorkspace;
    connect(&controller, &ApplicationController::windowCreated,
            &controller,
            [&](QQuickWindow *window, TerminalWorkspace *workspace) {
                observedWindow = window;
                observedWorkspace = workspace;
                if (deleteWorkspace) {
                    delete workspace;
                } else {
                    delete window;
                }
            });
    QSignalSpy failure(&controller,
                       &ApplicationController::windowCreationFailed);

    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(failure.count(), 1);
    QVERIFY(observedWindow.isNull());
    QVERIFY(observedWorkspace.isNull());
    QCOMPARE(controller.windowCount(), 0);
    QVERIFY(controller.windows().isEmpty());
    QCOMPARE(controller.lifetimeController()->registeredWindowCount(), 0);
}

void ApplicationControllerTest::creationObserverCannotDetachWorkspace()
{
    WindowFactoryHarness harness;
    ApplicationController controller(
        baseOptions(QDir::currentPath()), harness.factory(), false);
    QVERIFY(controller.startWithoutInitialWindow());

    QPointer<QQuickWindow> observedWindow;
    QPointer<TerminalWorkspace> observedWorkspace;
    connect(&controller, &ApplicationController::windowCreated,
            &controller,
            [&](QQuickWindow *window, TerminalWorkspace *workspace) {
                observedWindow = window;
                observedWorkspace = workspace;
                workspace->setParent(nullptr);
                QCOMPARE(workspace->window(), window);
            });
    QSignalSpy failure(&controller,
                       &ApplicationController::windowCreationFailed);

    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(failure.count(), 1);
    QVERIFY(observedWindow.isNull());
    QVERIFY(observedWorkspace.isNull());
    QCOMPARE(controller.windowCount(), 0);
    QCOMPARE(controller.lifetimeController()->registeredWindowCount(), 0);
}

void ApplicationControllerTest::workspaceLossRetiresOwningWindow()
{
    WindowFactoryHarness harness;
    ApplicationController controller(
        baseOptions(QDir::currentPath()), harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QPointer<QQuickWindow> window(initial->window);
    QPointer<TerminalWorkspace> workspace(initial->workspace);

    delete initial->workspace;
    QVERIFY(workspace.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(window.isNull(), 1000);
    QCOMPARE(controller.windowCount(), 0);
    QVERIFY(controller.windows().isEmpty());
    QCOMPARE(controller.lifetimeController()->registeredWindowCount(), 0);
}

void ApplicationControllerTest::suppressedStartupPreservesFirstSessionOptions()
{
    WindowFactoryHarness localHarness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.initialWindow = false;
    options.maximize = true;
    options.fullscreen = true;
    options.quitAfterLastWindowClosed = true;
    options.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(25);
    ApplicationController local(options, localHarness.factory(), false);

    QVERIFY(local.startWithoutInitialWindow());
    QCOMPARE(localHarness.calls, 0);
    QCOMPARE(local.windowCount(), 0);
    QVERIFY(!local.startWithoutInitialWindow());
    QVERIFY(!local.createInitialWindow().has_value());
    QTest::qWait(50);
    QVERIFY(!local.lifetimeController()->quitPending());
    QVERIFY(!local.lifetimeController()->hasRequestedQuit());

    LaunchOptions reloaded = options;
    reloaded.initialWindow = true;
    reloaded.fontSize = 19.0;
    reloaded.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("exit 0"),
    };
    reloaded.hold = true;
    local.applyLaunchOptions(reloaded);
    QCOMPARE(local.windowCount(), 0);
    QCOMPARE(localHarness.calls, 0);

    QVERIFY(local.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(local.windowCount(), 1, 1000);
    const ApplicationWindow firstWindow = local.windows().constFirst();
    const TerminalWorkspace *const first = firstWindow.workspace;
    QVERIFY(first->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!first->effectiveLaunchOptions().hold);
    TerminalController *const firstController =
        onlyController(firstWindow.workspace);
    QVERIFY(firstController != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(
        firstController->launchProgram(), reloaded.program, 2000);
    QVERIFY(firstController->launchHold());
    QCOMPARE(first->effectiveLaunchOptions().fontSize, 19.0);
    QCOMPARE(presentationWindowStates(firstWindow.window->windowStates()),
             Qt::WindowStates(Qt::WindowFullScreen));
    QCOMPARE(firstWindow.window->property(
                 "visibilityBeforeFullscreen").toInt(),
             static_cast<int>(QWindow::Maximized));

    reloaded.initialWindow = false;
    local.applyLaunchOptions(reloaded);
    QCOMPARE(local.windowCount(), 1);
    QVERIFY(local.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(local.windowCount(), 2, 1000);
    const TerminalWorkspace *const second =
        local.windows().constLast().workspace;
    QVERIFY(second->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!second->effectiveLaunchOptions().hold);
    TerminalController *const secondController =
        onlyController(local.windows().constLast().workspace);
    QVERIFY(secondController != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(secondController->sessionStarted(), 2000);
    QVERIFY(secondController->launchProgram().isEmpty());
    QVERIFY(!secondController->launchHold());

    WindowFactoryHarness activationHarness;
    ApplicationController activated(
        options, activationHarness.factory(), false);
    QVERIFY(activated.startWithoutInitialWindow());
    QVERIFY(activated.activateNoCommand());
    QCOMPARE(activated.windowCount(), 1);
    const ApplicationWindow activatedFirstWindow =
        activated.windows().constFirst();
    const TerminalWorkspace *const activatedFirst =
        activatedFirstWindow.workspace;
    QVERIFY(activatedFirst->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!activatedFirst->effectiveLaunchOptions().hold);
    TerminalController *const activatedFirstController =
        onlyController(activatedFirstWindow.workspace);
    QVERIFY(activatedFirstController != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(
        activatedFirstController->launchProgram(), options.program, 2000);
    QVERIFY(activatedFirstController->launchHold());
    QCOMPARE(presentationWindowStates(
                 activatedFirstWindow.window->windowStates()),
             Qt::WindowStates(Qt::WindowFullScreen));
    QCOMPARE(activatedFirstWindow.window->property(
                 "visibilityBeforeFullscreen").toInt(),
             static_cast<int>(QWindow::Maximized));
    QVERIFY(activated.activateNoCommand());
    QCOMPARE(activated.windowCount(), 2);
    QVERIFY(activated.windows().constLast().workspace
                ->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!activated.windows().constLast().workspace
                 ->effectiveLaunchOptions().hold);
    TerminalController *const activatedSecondController =
        onlyController(activated.windows().constLast().workspace);
    QVERIFY(activatedSecondController != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(
        activatedSecondController->sessionStarted(), 2000);
    QVERIFY(activatedSecondController->launchProgram().isEmpty());
    QVERIFY(!activatedSecondController->launchHold());
}

void ApplicationControllerTest::failedLazyCreationDoesNotConsumeFirstSession()
{
    WindowFactoryHarness harness;
    harness.failOnCall = 1;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.initialWindow = false;
    ApplicationController controller(options, harness.factory(), false);
    QVERIFY(controller.startWithoutInitialWindow());

    QSignalSpy failure(&controller,
                       &ApplicationController::windowCreationFailed);
    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(failure.count(), 1);
    QCOMPARE(harness.calls, 1);
    QCOMPARE(controller.windowCount(), 0);

    QVERIFY(controller.activateNoCommand());
    QCOMPARE(harness.calls, 2);
    QCOMPARE(controller.windowCount(), 1);
    const ApplicationWindow actual = controller.windows().constFirst();
    QVERIFY(actual.workspace->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!actual.workspace->effectiveLaunchOptions().hold);
    TerminalController *const terminal = onlyController(actual.workspace);
    QVERIFY(terminal != nullptr);
    QCOMPARE(terminal->launchProgram(), options.program);
    QVERIFY(terminal->launchHold());
}

void ApplicationControllerTest::terminalInitializationFailurePromotesNextSession()
{
    const InitialSessionCoordinator::Payload initialPayload{
        .program = {QStringLiteral("/bin/true")},
        .hold = true,
    };
    const auto coordinator =
        std::make_shared<InitialSessionCoordinator>(initialPayload);

    TerminalSessionLaunchOptions rejectedOptions;
    rejectedOptions.workingDirectory = QDir::currentPath();
    rejectedOptions.runtime.appearance.foregroundColor = QColor{};
    TerminalController rejected(rejectedOptions, nullptr, coordinator);

    TerminalSessionLaunchOptions acceptedOptions;
    acceptedOptions.workingDirectory = QDir::currentPath();
    TerminalController accepted(acceptedOptions, nullptr, coordinator);

    QSignalSpy rejectedError(&rejected,
                             &TerminalController::errorOccurred);
    QSignalSpy rejectedExit(&rejected,
                            &TerminalController::sessionExited);
    QSignalSpy acceptedError(&accepted,
                             &TerminalController::errorOccurred);

    QVERIFY(rejected.startSession());
    QVERIFY(accepted.startSession());

    QTRY_COMPARE_WITH_TIMEOUT(rejectedError.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(rejectedExit.count(), 1, 2000);
    QVERIFY(rejectedError.constFirst().constFirst().toString().contains(
        QStringLiteral("Failed to initialize libghostty-vt")));
    QTRY_VERIFY_WITH_TIMEOUT(accepted.sessionStarted(), 2000);
    QCOMPARE(accepted.launchProgram(), initialPayload.program);
    QVERIFY(accepted.launchHold());
    QTRY_COMPARE_WITH_TIMEOUT(
        coordinator->state(), InitialSessionCoordinator::State::Consumed,
        2000);
    QVERIFY(acceptedError.isEmpty());
}

void ApplicationControllerTest::waitingControllersCancelWithoutCreatingWorkers()
{
    const auto coordinator = std::make_shared<InitialSessionCoordinator>();
    const InitialSessionCoordinator::RequestResult holder =
        coordinator->request();
    QVERIFY(holder.granted());

    TerminalSessionLaunchOptions options;
    options.workingDirectory = QDir::currentPath();

    auto cancelled =
        std::make_unique<TerminalController>(options, nullptr, coordinator);
    QVERIFY(cancelled->startSession());
    QVERIFY(!cancelled->sessionStarted());
    QVERIFY(cancelled->findChild<QThread *>() == nullptr);
    cancelled->beginShutdown();
    QVERIFY(!cancelled->sessionStarted());
    QVERIFY(cancelled->findChild<QThread *>() == nullptr);

    {
        auto destroyed =
            std::make_unique<TerminalController>(options, nullptr, coordinator);
        QVERIFY(destroyed->startSession());
        QVERIFY(!destroyed->sessionStarted());
        QVERIFY(destroyed->findChild<QThread *>() == nullptr);
    }

    QVERIFY(coordinator->release(holder.ticket));
    QCOMPARE(coordinator->state(), InitialSessionCoordinator::State::Available);
    const InitialSessionCoordinator::RequestResult next =
        coordinator->request();
    QVERIFY(next.granted());
    QVERIFY(coordinator->release(next.ticket));
}

void ApplicationControllerTest::immediateTabWinsInitialSessionLease()
{
    const ScopedEnvironmentVariable shell(
        QByteArrayLiteral("SHELL"), QByteArrayLiteral("/bin/sh"));
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/initial-session-tab-XXXXXX")));
    QVERIFY(temporary.isValid());
    const QString marker = temporary.filePath(QStringLiteral("starts"));

    WindowFactoryHarness harness;
    harness.createTabAtPresentation = true;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.fullscreen = true;
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("printf 'initial\\n' >> \"$0\""), marker,
    };

    ApplicationController controller(options, harness.factory(), false);
    const auto created = controller.createInitialWindow();
    QVERIFY(created.has_value());

    TerminalPane *deferredPane = nullptr;
    TerminalPane *immediatePane = nullptr;
    const QList<TerminalPane *> panes =
        created->workspace->findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 2);
    for (TerminalPane *const pane : panes) {
        if (terminalPaneRenderProbe(pane).initialGeometry.has_value()) {
            deferredPane = pane;
        } else {
            immediatePane = pane;
        }
    }
    QVERIFY(deferredPane != nullptr);
    QVERIFY(immediatePane != nullptr);

    TerminalController *const immediate =
        immediatePane->findChild<TerminalController *>();
    TerminalController *const deferred =
        deferredPane->findChild<TerminalController *>();
    QVERIFY(immediate != nullptr);
    QVERIFY(deferred != nullptr);
    QVERIFY(immediate->sessionStarted());
    QCOMPARE(immediate->launchProgram(), options.program);
    QVERIFY(immediate->launchHold());
    QCOMPARE(immediatePane->title(), QStringLiteral("sh"));

    QTRY_VERIFY_WITH_TIMEOUT(deferred->sessionStarted(), 2000);
    QVERIFY(deferred->launchProgram().isEmpty());
    QVERIFY(!deferred->launchHold());
    QCOMPARE(deferredPane->title(), QStringLiteral("Terminal"));
    const QStringList titles = created->workspace->tabTitles();
    QVERIFY(titles.contains(QStringLiteral("sh")));
    QVERIFY(titles.contains(QStringLiteral("Terminal")));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 2000);
    QFile starts(marker);
    QVERIFY(starts.open(QIODevice::ReadOnly));
    QCOMPARE(starts.readAll(), QByteArrayLiteral("initial\n"));
}

void ApplicationControllerTest::reverseExposureGrantsInitialSessionToFirstStarter()
{
    const ScopedEnvironmentVariable shell(
        QByteArrayLiteral("SHELL"), QByteArrayLiteral("/bin/sh"));
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/initial-session-windows-XXXXXX")));
    QVERIFY(temporary.isValid());
    const QString marker = temporary.filePath(QStringLiteral("starts"));

    int factoryCalls = 0;
    bool suppressDeferredExposure = true;
    ApplicationController::WindowFactory factory = [&]
        -> std::expected<ApplicationWindow, QString> {
        ++factoryCalls;
        auto *window = new QQuickWindow;
        window->resize(800, 500);
        auto *workspace = new TerminalWorkspace(window->contentItem());
        workspace->setParentItem(window->contentItem());
        workspace->setSize(window->size());
        if (factoryCalls == 1) {
            connect(window, &QWindow::visibleChanged, window,
                    [window, &suppressDeferredExposure](bool visible) {
                        if (visible && suppressDeferredExposure) {
                            window->hide();
                        }
                    });
        }
        return ApplicationWindow{window, workspace};
    };

    LaunchOptions options = baseOptions(QDir::currentPath());
    options.fullscreen = true;
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("printf 'initial\\n' >> \"$0\""), marker,
    };
    ApplicationController controller(options, std::move(factory), false);

    const auto deferredWindow = controller.createInitialWindow();
    QVERIFY(deferredWindow.has_value());
    TerminalController *const deferredController =
        onlyController(deferredWindow->workspace);
    QVERIFY(deferredController != nullptr);
    QVERIFY(!deferredController->sessionStarted());

    QVERIFY(controller.activateNoCommand());
    QCOMPARE(factoryCalls, 2);
    const ApplicationWindow winningWindow = controller.windows().constLast();
    TerminalController *const winningController =
        onlyController(winningWindow.workspace);
    QVERIFY(winningController != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(winningController->sessionStarted(), 2000);
    QCOMPARE(winningController->launchProgram(), options.program);
    QVERIFY(winningController->launchHold());
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 2000);

    // The pane scheduler normally supplies this geometry after exposure. Call
    // the same public one-shot boundary directly so the test controls root
    // start order independently of the offscreen compositor's window limit.
    suppressDeferredExposure = false;
    QVERIFY(deferredController->startSession(TerminalSessionGeometry{
        .columns = 80,
        .rows = 24,
        .cellWidthPixels = 8,
        .cellHeightPixels = 16,
        .surfaceWidthPixels = 640,
        .surfaceHeightPixels = 384,
    }));
    QTRY_VERIFY_WITH_TIMEOUT(deferredController->sessionStarted(), 2000);
    QVERIFY(deferredController->launchProgram().isEmpty());
    QVERIFY(!deferredController->launchHold());

    QFile starts(marker);
    QVERIFY(starts.open(QIODevice::ReadOnly));
    QCOMPARE(starts.readAll(), QByteArrayLiteral("initial\n"));
}

void ApplicationControllerTest::ordinaryInheritanceDoesNotWaitForFirstSession()
{
    QTemporaryDir directories(QDir::current().filePath(
        QStringLiteral("tmp/pre-session-inheritance-XXXXXX")));
    QVERIFY(directories.isValid());
    const QString sourceDirectory =
        QDir(directories.path()).filePath(QStringLiteral("source"));
    const QString configuredDirectory =
        QDir(directories.path()).filePath(QStringLiteral("configured"));
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(QDir().mkpath(configuredDirectory));

    int factoryCalls = 0;
    ApplicationController::WindowFactory factory = [&]
        -> std::expected<ApplicationWindow, QString> {
        ++factoryCalls;
        auto *window = new QQuickWindow;
        window->resize(800, 500);
        auto *workspace = new TerminalWorkspace(window->contentItem());
        workspace->setParentItem(window->contentItem());
        workspace->setSize(window->size());
        if (factoryCalls == 1) {
            connect(window, &QWindow::visibleChanged, window,
                    [window](bool visible) {
                        if (visible) window->hide();
                    });
        }
        return ApplicationWindow{window, workspace};
    };

    LaunchOptions options = baseOptions(sourceDirectory);
    options.fontSize = 13.0;
    options.maximize = true;
    ApplicationController controller(options, std::move(factory), false);
    const auto deferred = controller.createInitialWindow();
    QVERIFY(deferred.has_value());
    TerminalPane *const sourcePane = onlyPane(deferred->workspace);
    TerminalController *const sourceController =
        onlyController(deferred->workspace);
    QVERIFY(sourcePane != nullptr);
    QVERIFY(sourceController != nullptr);
    QVERIFY(!sourceController->sessionStarted());

    LaunchOptions reloaded = options;
    reloaded.workingDirectory = configuredDirectory;
    reloaded.fontSize = 11.0;
    reloaded.maximize = false;
    controller.applyLaunchOptions(reloaded);
    QVERIFY(sourcePane->executeConfiguredAction(
        QStringLiteral("set_font_size:18")));
    QVERIFY(sourcePane->executeConfiguredAction(
        QStringLiteral("new_window")));

    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 2000);
    const ApplicationWindow inherited = controller.windows().constLast();
    QCOMPARE(inherited.workspace->effectiveLaunchOptions().workingDirectory,
             sourceDirectory);
    QCOMPARE(inherited.workspace->effectiveLaunchOptions().fontSize, 18.0);
    QVERIFY(inherited.workspace->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!inherited.workspace->effectiveLaunchOptions().hold);

    TerminalController *const winner = onlyController(inherited.workspace);
    QVERIFY(winner != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(winner->sessionStarted(), 2000);
    QCOMPARE(winner->launchProgram(), reloaded.program);
    QVERIFY(winner->launchHold());
    QVERIFY(!sourceController->sessionStarted());
}

void ApplicationControllerTest::deferredRequestsRemainOrderedAcrossStart()
{
    QTemporaryDir temporary(QDir::current().filePath(
        QStringLiteral("tmp/deferred-request-order-XXXXXX")));
    QVERIFY(temporary.isValid());
    const QString inputPath = temporary.filePath(QStringLiteral("input"));

    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.maximize = true;
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral(
            "IFS= read -r line; printf '%s' \"$line\" > \"$0\"; "
            "exec sleep 30"),
        inputPath,
    };

    ApplicationController controller(options, harness.factory(), false);
    const auto created = controller.createInitialWindow();
    QVERIFY(created.has_value());
    TerminalController *const terminal = onlyController(created->workspace);
    QVERIFY(terminal != nullptr);
    QVERIFY(!terminal->sessionStarted());
    QVERIFY(terminal->findChild<QThread *>() == nullptr);

    terminal->setReadOnly(true);
    terminal->sendRawText(QByteArrayLiteral("blocked\n"));
    terminal->setReadOnly(false);
    terminal->sendRawText(QByteArrayLiteral("delivered\n"));

    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(inputPath), 2000);
    QFile input(inputPath);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QCOMPARE(input.readAll(), QByteArrayLiteral("delivered"));
    QVERIFY(terminal->sessionStarted());
    QVERIFY(terminal->findChild<QThread *>() != nullptr);
}

void ApplicationControllerTest::ordinaryCloseUsesOnlyTheFinalWindowForLifetimePolicy()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.quitAfterLastWindowClosed = true;
    options.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(150);
    ApplicationController controller(options, harness.factory(), false);
    QVERIFY(controller.createInitialWindow().has_value());
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);

    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    const QVector<ApplicationWindow> windows = controller.windows();
    closeWorkspace(windows.constFirst().workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    QTest::qWait(200);
    QCOMPARE(quit.count(), 0);
    QVERIFY(controller.lifetimeController()->hasOpenWindow());

    closeWorkspace(windows.constLast().workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
}

void ApplicationControllerTest::successfulReplacementCancelsDelayedQuit()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.quitAfterLastWindowClosed = true;
    options.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(150);
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());

    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    closeWorkspace(initial->workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QVERIFY(controller.lifetimeController()->quitPending());

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    QVERIFY(!controller.lifetimeController()->quitPending());
    QTest::qWait(200);
    QCOMPARE(quit.count(), 0);

    QVERIFY(controller.dispatch(ApplicationAction::Quit));
    QCOMPARE(quit.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
}

void ApplicationControllerTest::sourceLessActivationMatchesUpstreamInheritance()
{
    QTemporaryDir directories(
        QDir::current().filePath(
            QStringLiteral("tmp/application-activation-XXXXXX")));
    QVERIFY(directories.isValid());
    const QString focusedDirectory =
        QDir(directories.path()).filePath(QStringLiteral("focused"));
    const QString configuredDirectory =
        QDir(directories.path()).filePath(QStringLiteral("configured"));
    QVERIFY(QDir().mkpath(focusedDirectory));
    QVERIFY(QDir().mkpath(configuredDirectory));

    WindowFactoryHarness harness;
    LaunchOptions initialOptions = baseOptions(focusedDirectory);
    initialOptions.fontSize = 13.0;
    ApplicationController controller(
        initialOptions, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    TerminalPane *const focusedPane = onlyPane(initial->workspace);
    QVERIFY(focusedPane != nullptr);
    QVERIFY(focusedPane->executeConfiguredAction(
        QStringLiteral("set_font_size:18")));

    LaunchOptions currentOptions = initialOptions;
    currentOptions.workingDirectory = configuredDirectory;
    currentOptions.fontSize = 11.0;
    controller.applyLaunchOptions(currentOptions);

    // Registration alone is not focus. With no pane ever focused, source-less
    // activation keeps the configured cwd instead of choosing an arbitrary
    // live workspace.
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 2);
    TerminalWorkspace *const noFocus =
        controller.windows().constLast().workspace;
    QCOMPARE(noFocus->effectiveLaunchOptions().workingDirectory,
             configuredDirectory);

    closeWorkspace(noFocus);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!focusedPane->isRunning(), 1000);
    initial->window->requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(
        QGuiApplication::focusWindow() == initial->window, 1000);
    focusedPane->setFocus(false);
    focusedPane->forceActiveFocus(Qt::OtherFocusReason);
    QTRY_VERIFY_WITH_TIMEOUT(focusedPane->hasActiveFocus(), 1000);
    QCOMPARE(controller.activeWorkspace(), initial->workspace);
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 2);
    const TerminalWorkspace *const inherited =
        controller.windows().constLast().workspace;
    QCOMPARE(inherited->effectiveLaunchOptions().workingDirectory,
             focusedDirectory);
    QCOMPARE(inherited->effectiveLaunchOptions().fontSize, 11.0);
    QVERIFY(inherited->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!inherited->effectiveLaunchOptions().hold);

    // If the last-focused workspace disappears while another live window is
    // hidden, the remaining workspace is not an implicit focus substitute.
    QQuickWindow *const remainingWindow =
        controller.windows().constLast().window;
    remainingWindow->setFlag(Qt::WindowDoesNotAcceptFocus, true);
    remainingWindow->hide();
    initial->window->requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(
        QGuiApplication::focusWindow() == initial->window, 1000);
    QSignalSpy workspaceActivation(
        initial->workspace, &TerminalWorkspace::workspaceActivated);
    focusedPane->setFocus(false);
    focusedPane->forceActiveFocus(Qt::OtherFocusReason);
    QTRY_VERIFY_WITH_TIMEOUT(focusedPane->hasActiveFocus(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(workspaceActivation.count(), 1, 1000);
    QPointer<QQuickWindow> destroyedWindow(initial->window);
    delete initial->window;
    QVERIFY(destroyedWindow.isNull());
    QCOMPARE(controller.windowCount(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(QGuiApplication::focusWindow() == nullptr, 1000);
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 2);
    const TerminalWorkspace *const staleFocus =
        controller.windows().constLast().workspace;
    QCOMPARE(staleFocus->effectiveLaunchOptions().workingDirectory,
             configuredDirectory);

    currentOptions.windowInheritWorkingDirectory = false;
    controller.applyLaunchOptions(currentOptions);
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 3);
    const TerminalWorkspace *const configured =
        controller.windows().constLast().workspace;
    QCOMPARE(configured->effectiveLaunchOptions().workingDirectory,
             configuredDirectory);
    QCOMPARE(configured->effectiveLaunchOptions().fontSize, 11.0);
}

void ApplicationControllerTest::sourceLessActivationCancelsQuitAndReportsFailure()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.quitAfterLastWindowClosed = true;
    options.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(150);
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());

    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    closeWorkspace(initial->workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QVERIFY(controller.lifetimeController()->quitPending());
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 1);
    QVERIFY(!controller.lifetimeController()->quitPending());
    QTest::qWait(200);
    QCOMPARE(quit.count(), 0);

    WindowFactoryHarness failingHarness;
    failingHarness.failOnCall = 2;
    LaunchOptions residentOptions = baseOptions(QDir::currentPath());
    ApplicationController failing(
        residentOptions, failingHarness.factory(), false);
    QVERIFY(failing.createInitialWindow().has_value());
    QSignalSpy failure(&failing,
                       &ApplicationController::windowCreationFailed);
    QVERIFY(!failing.activateNoCommand());
    QCOMPARE(failure.count(), 1);
    QCOMPARE(failing.windowCount(), 1);
}

void ApplicationControllerTest::sourceLessActivationProjectsDesktopContext()
{
    const bool tokenWasSet =
        qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN");
    const bool startupWasSet =
        qEnvironmentVariableIsSet("DESKTOP_STARTUP_ID");
    const QByteArray previousToken = qgetenv("XDG_ACTIVATION_TOKEN");
    const QByteArray previousStartup = qgetenv("DESKTOP_STARTUP_ID");
    (void)qunsetenv("XDG_ACTIVATION_TOKEN");
    (void)qunsetenv("DESKTOP_STARTUP_ID");
    const auto restoreEnvironment = qScopeGuard([&] {
        if (tokenWasSet) {
            (void)qputenv("XDG_ACTIVATION_TOKEN", previousToken);
        } else {
            (void)qunsetenv("XDG_ACTIVATION_TOKEN");
        }
        if (startupWasSet) {
            (void)qputenv("DESKTOP_STARTUP_ID", previousStartup);
        } else {
            (void)qunsetenv("DESKTOP_STARTUP_ID");
        }
    });

    QByteArray tokenDuringShow;
    QByteArray startupDuringShow;
    ApplicationController::WindowFactory factory = [&]()
        -> std::expected<ApplicationWindow, QString> {
        auto *window = new QQuickWindow;
        auto *workspace = new TerminalWorkspace(window->contentItem());
        workspace->setParentItem(window->contentItem());
        connect(window, &QWindow::visibleChanged, window,
                [&](bool visible) {
                    if (!visible) return;
                    tokenDuringShow = qgetenv("XDG_ACTIVATION_TOKEN");
                    startupDuringShow = qgetenv("DESKTOP_STARTUP_ID");
                });
        return ApplicationWindow{window, workspace};
    };

    ApplicationController controller(
        baseOptions(QDir::currentPath()), std::move(factory), false);
    QVERIFY(controller.startWithoutInitialWindow());
    QVERIFY(controller.activateNoCommand({
        .xdgActivationToken = QStringLiteral("controller-token"),
        .desktopStartupId = QStringLiteral("controller-startup"),
    }));
    QCOMPARE(tokenDuringShow, QByteArrayLiteral("controller-token"));
    QCOMPARE(startupDuringShow, QByteArrayLiteral("controller-startup"));
    QVERIFY(!qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN"));
    QVERIFY(!qEnvironmentVariableIsSet("DESKTOP_STARTUP_ID"));
}

void ApplicationControllerTest::sourceLessActivationRequiresStartupDecision()
{
    WindowFactoryHarness harness;
    ApplicationController controller(
        baseOptions(QDir::currentPath()), harness.factory(), false);
    QSignalSpy failure(&controller,
                       &ApplicationController::windowCreationFailed);

    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(failure.count(), 1);
    QCOMPARE(harness.calls, 0);
    QVERIFY(controller.startWithoutInitialWindow());
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(harness.calls, 1);
    QCOMPARE(failure.count(), 1);
}

void ApplicationControllerTest::reentrantWindowCreationIsRejected()
{
    int factoryCalls = 0;
    ApplicationController *controller = nullptr;
    std::optional<bool> nestedResult;
    QByteArray tokenAfterNestedAttempt;
    ApplicationController::WindowFactory factory = [&]()
        -> std::expected<ApplicationWindow, QString> {
        ++factoryCalls;
        auto *window = new QQuickWindow;
        auto *workspace = new TerminalWorkspace(window->contentItem());
        workspace->setParentItem(window->contentItem());
        connect(window, &QWindow::visibleChanged, window,
                [&](bool visible) {
                    if (!visible || nestedResult.has_value()) return;
                    nestedResult = controller->activateNoCommand({
                        .xdgActivationToken =
                            QStringLiteral("nested-token"),
                    });
                    tokenAfterNestedAttempt =
                        qgetenv("XDG_ACTIVATION_TOKEN");
                });
        return ApplicationWindow{window, workspace};
    };

    ApplicationController ownedController(
        baseOptions(QDir::currentPath()), std::move(factory), false);
    controller = &ownedController;
    QSignalSpy failure(&ownedController,
                       &ApplicationController::windowCreationFailed);

    QVERIFY(ownedController.startWithoutInitialWindow());
    QVERIFY(ownedController.activateNoCommand({
        .xdgActivationToken = QStringLiteral("outer-token"),
    }));
    QVERIFY(nestedResult.has_value());
    QVERIFY(!*nestedResult);
    QCOMPARE(tokenAfterNestedAttempt, QByteArrayLiteral("outer-token"));
    QCOMPARE(factoryCalls, 1);
    QCOMPARE(failure.count(), 1);
    QCOMPARE(ownedController.windowCount(), 1);
    QVERIFY(!qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN"));
}

void ApplicationControllerTest::showDestructionCannotLeaveHalfRegisteredWindow()
{
    QPointer<QQuickWindow> observedWindow;
    QPointer<TerminalWorkspace> observedWorkspace;
    bool sessionStartedBeforeDestruction = true;
    ApplicationController::WindowFactory factory = [&]()
        -> std::expected<ApplicationWindow, QString> {
        observedWindow = new QQuickWindow;
        observedWorkspace = new TerminalWorkspace(
            observedWindow->contentItem());
        observedWorkspace->setParentItem(observedWindow->contentItem());
        connect(observedWindow, &QWindow::visibleChanged, observedWindow,
                [workspace = observedWorkspace,
                 &sessionStartedBeforeDestruction](bool visible) {
                    if (visible && workspace != nullptr) {
                        TerminalController *const terminal =
                            workspace->findChild<TerminalController *>();
                        sessionStartedBeforeDestruction = terminal != nullptr
                            && terminal->sessionStarted();
                        delete workspace.data();
                    }
                });
        return ApplicationWindow{observedWindow, observedWorkspace};
    };

    LaunchOptions options = baseOptions(QDir::currentPath());
    options.maximize = true;
    ApplicationController controller(options, std::move(factory), false);
    QVERIFY(controller.startWithoutInitialWindow());
    QSignalSpy failure(&controller,
                       &ApplicationController::windowCreationFailed);

    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(failure.count(), 1);
    QVERIFY(!sessionStartedBeforeDestruction);
    QVERIFY(observedWindow.isNull());
    QVERIFY(observedWorkspace.isNull());
    QCOMPARE(controller.windowCount(), 0);
    QVERIFY(controller.windows().isEmpty());
    QCOMPARE(controller.lifetimeController()->registeredWindowCount(), 0);
}

void ApplicationControllerTest::failedInitialPresentationPreservesFirstSession()
{
    int factoryCalls = 0;
    ApplicationController::WindowFactory factory = [&]()
        -> std::expected<ApplicationWindow, QString> {
        ++factoryCalls;
        auto *window = new QQuickWindow;
        window->resize(800, 500);
        auto *workspace = new TerminalWorkspace(window->contentItem());
        workspace->setParentItem(window->contentItem());
        workspace->setSize(window->size());
        if (factoryCalls == 1) {
            connect(window, &QWindow::visibleChanged, window,
                    [guardedWorkspace = QPointer(workspace)](bool visible) {
                        if (visible && guardedWorkspace != nullptr) {
                            delete guardedWorkspace.data();
                        }
                    });
        }
        return ApplicationWindow{window, workspace};
    };

    LaunchOptions options = baseOptions(QDir::currentPath());
    options.maximize = true;
    ApplicationController controller(options, std::move(factory), false);
    const auto failed = controller.createInitialWindow();
    QVERIFY(!failed.has_value());
    QCOMPARE(factoryCalls, 1);

    const auto retry = controller.createInitialWindow();
    QVERIFY(!retry.has_value());
    QVERIFY(retry.error().contains(QStringLiteral("already handled")));
    QCOMPARE(factoryCalls, 1);

    QVERIFY(controller.activateNoCommand());
    QCOMPARE(factoryCalls, 2);
    const QVector<ApplicationWindow> windows = controller.windows();
    QCOMPARE(windows.size(), 1);
    QVERIFY(windows.constFirst().workspace
                ->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!windows.constFirst().workspace
                 ->effectiveLaunchOptions().hold);
    TerminalController *const terminal =
        onlyController(windows.constFirst().workspace);
    QVERIFY(terminal != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(terminal->launchProgram(), options.program, 2000);
    QVERIFY(terminal->launchHold());
}

void ApplicationControllerTest::explicitQuitAggregatesEveryWindowIntoOneConfirmation()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(options, harness.factory(), false);
    const auto first = controller.createInitialWindow();
    QVERIFY(first.has_value());
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    const ApplicationWindow second = controller.windows().constLast();
    QCOMPARE(controller.activeWorkspace(), second.workspace);

    TerminalPane *const guardedBackground = onlyPane(first->workspace);
    QVERIFY(guardedBackground != nullptr);
    QVERIFY(guardedBackground->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QSignalSpy firstConfirmation(
        first->workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy secondConfirmation(
        second.workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy firstClose(
        first->workspace, &TerminalWorkspace::windowCloseApproved);
    QSignalSpy secondClose(
        second.workspace, &TerminalWorkspace::windowCloseApproved);
    QSignalSpy committed(&controller,
                         &ApplicationController::applicationQuitCommitted);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    QSignalSpy creationFailure(
        &controller, &ApplicationController::windowCreationFailed);

    QVERIFY(controller.dispatch(ApplicationAction::Quit, second.workspace));
    QCOMPARE(firstConfirmation.count(), 0);
    QCOMPARE(secondConfirmation.count(), 1);
    QCOMPARE(firstClose.count(), 0);
    QCOMPARE(secondClose.count(), 0);
    QCOMPARE(secondConfirmation.constFirst().at(1).toString(),
             QStringLiteral(
                 "A terminal window contains a read-only pane. Quit the application?"));
    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(creationFailure.count(), 1);
    QCOMPARE(controller.windowCount(), 2);

    const quint64 cancelledId =
        secondConfirmation.constFirst().constFirst().toULongLong();
    second.workspace->cancelClose(cancelledId);
    QCOMPARE(controller.windowCount(), 2);
    QCOMPARE(firstClose.count(), 0);
    QCOMPARE(secondClose.count(), 0);
    QCOMPARE(committed.count(), 0);

    QVERIFY(controller.dispatch(ApplicationAction::Quit, second.workspace));
    QCOMPARE(secondConfirmation.count(), 2);
    const quint64 acceptedId =
        secondConfirmation.constLast().constFirst().toULongLong();
    QVERIFY(acceptedId != cancelledId);
    second.workspace->confirmClose(acceptedId);
    QCOMPARE(firstClose.count(), 1);
    QCOMPARE(secondClose.count(), 1);
    QCOMPARE(committed.count(), 1);
    QCOMPARE(quit.count(), 1);
    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(creationFailure.count(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
}

void ApplicationControllerTest::pendingQuitRehostsAfterItsWindowDisappears()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(options, harness.factory(), false);
    const auto first = controller.createInitialWindow();
    QVERIFY(first.has_value());
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    const ApplicationWindow second = controller.windows().constLast();

    TerminalPane *const guardedBackground = onlyPane(first->workspace);
    QVERIFY(guardedBackground != nullptr);
    QVERIFY(guardedBackground->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QSignalSpy firstConfirmation(
        first->workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy secondConfirmation(
        second.workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy committed(&controller,
                         &ApplicationController::applicationQuitCommitted);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);

    QVERIFY(controller.dispatch(ApplicationAction::Quit, second.workspace));
    QCOMPARE(secondConfirmation.count(), 1);
    QCOMPARE(firstConfirmation.count(), 0);

    QPointer<QQuickWindow> removedHost(second.window);
    delete second.window;
    QVERIFY(removedHost.isNull());
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(firstConfirmation.count(), 1, 1000);
    QCOMPARE(committed.count(), 0);
    QCOMPARE(firstConfirmation.constFirst().at(1).toString(),
             QStringLiteral(
                 "A terminal window contains a read-only pane. Quit the application?"));

    const quint64 confirmationId =
        firstConfirmation.constFirst().constFirst().toULongLong();
    first->workspace->confirmClose(confirmationId);
    QCOMPARE(committed.count(), 1);
    QCOMPARE(quit.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
}

void ApplicationControllerTest::failedReplacementLeavesDelayedQuitArmed()
{
    WindowFactoryHarness harness;
    harness.failOnCall = 2;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.quitAfterLastWindowClosed = true;
    options.quitAfterLastWindowClosedDelay = std::chrono::milliseconds(200);
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());

    QSignalSpy failure(&controller,
                       &ApplicationController::windowCreationFailed);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    closeWorkspace(initial->workspace);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QVERIFY(controller.lifetimeController()->quitPending());

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(failure.count(), 1, 1000);
    QVERIFY(controller.lifetimeController()->quitPending());
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
}

QTEST_MAIN(ApplicationControllerTest)

#include "test_application_controller.moc"
