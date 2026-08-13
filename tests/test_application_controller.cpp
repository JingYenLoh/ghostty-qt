#include "application_controller.h"
#include "ghostty_application_ipc.h"
#include "terminal_cell_metrics.h"
#include "terminal_controller.h"
#include "terminal_geometry.h"
#include "terminal_pane.h"
#include "terminal_pane_render_probe_p.h"
#include "terminal_workspace.h"
#include "window_ui_controller.h"

#include <LayerShellQt/Window>

#include <QAccessible>
#include <QClipboard>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QScreen>
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
        Qt::WindowFlags flags;
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
                                         window
                                             ->property(
                                                 "visibilityBeforeFullscreen")
                                             .toInt(),
                                     .size = window->size(),
                                     .minimumSize = window->minimumSize(),
                                     .flags = window->flags(),
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
    options.keybindSource = GhosttyKeybindSource::structured({});
    return options;
}

TerminalTypography typographyWithPointSize(
    TerminalTypography typography, double pointSize)
{
    typography.pointSize = pointSize;
    return typography;
}

TerminalTypography sampleTypography(double pointSize)
{
    TerminalTypography typography;
    typography.pointSize = pointSize;
    typography.face(TerminalFontRole::Regular) = {
        .families = {
            QStringLiteral("Monospace"),
            QStringLiteral("DejaVu Sans Mono"),
        },
        .style = TerminalFontStyles::Named{QStringLiteral("Regular")},
    };
    typography.face(TerminalFontRole::Bold) = {
        .families = {
            QStringLiteral("DejaVu Sans Mono"),
            QStringLiteral("Monospace"),
        },
        .style = TerminalFontStyles::Named{QStringLiteral("Bold")},
    };
    typography.face(TerminalFontRole::Italic) = {
        .families = {
            QStringLiteral("Liberation Mono"),
            QStringLiteral("Monospace"),
        },
        .style = TerminalFontStyles::Disabled{},
    };
    typography.face(TerminalFontRole::BoldItalic) = {
        .families = {
            QStringLiteral("Noto Sans Mono"),
            QStringLiteral("Monospace"),
        },
        .style = TerminalFontStyles::Automatic{},
    };

    typography.metricModifiers[TerminalMetric::CellWidth] =
        TerminalMetricModifiers::Percentage{1.05};
    typography.metricModifiers[TerminalMetric::CellHeight] =
        TerminalMetricModifiers::Percentage{1.10};
    typography.metricModifiers[TerminalMetric::FontBaseline] =
        TerminalMetricModifiers::Absolute{-1};
    typography.metricModifiers[TerminalMetric::UnderlinePosition] =
        TerminalMetricModifiers::Absolute{1};
    typography.metricModifiers[TerminalMetric::UnderlineThickness] =
        TerminalMetricModifiers::Absolute{1};
    typography.metricModifiers[TerminalMetric::StrikethroughPosition] =
        TerminalMetricModifiers::Percentage{0.95};
    typography.metricModifiers[TerminalMetric::StrikethroughThickness] =
        TerminalMetricModifiers::Absolute{1};
    typography.metricModifiers[TerminalMetric::OverlinePosition] =
        TerminalMetricModifiers::Absolute{-1};
    typography.metricModifiers[TerminalMetric::OverlineThickness] =
        TerminalMetricModifiers::Absolute{1};
    typography.metricModifiers[TerminalMetric::CursorThickness] =
        TerminalMetricModifiers::Percentage{1.25};
    typography.metricModifiers[TerminalMetric::CursorHeight] =
        TerminalMetricModifiers::Percentage{0.90};
    return typography;
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

WindowUiController *windowUiController(QQuickWindow *window)
{
    return window != nullptr
        ? qobject_cast<WindowUiController *>(
              window->property("uiController").value<QObject *>())
        : nullptr;
}

int paletteRowWithTitle(const CommandPaletteModel &model, const QString &title)
{
    for (int row = 0; row < model.count(); ++row) {
        if (model.data(model.index(row, 0), CommandPaletteModel::TitleRole)
                .toString()
            == title) {
            return row;
        }
    }
    return -1;
}

std::optional<ApplicationWindow>
quickTerminalWindow(const ApplicationController &controller)
{
    const QVector<ApplicationWindow> windows = controller.windows();
    const auto quick =
        std::ranges::find_if(windows, [](const ApplicationWindow &window) {
            return window.window != nullptr
                && window.window->property("quickTerminal").toBool();
        });
    return quick == windows.cend() ? std::nullopt
                                   : std::optional<ApplicationWindow>{*quick};
}

void sendCtrlKPressAndRelease(TerminalPane *pane)
{
    QKeyEvent press(QEvent::KeyPress, Qt::Key_K,
                    Qt::ControlModifier, QString(QChar(0x0b)));
    QCoreApplication::sendEvent(pane, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_K,
                      Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &release);
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
    void consumesDeprecatedCloseAllWindowsWithoutMutation();
    void configuresInitialWindowStateBeforePresentation_data();
    void configuresInitialWindowStateBeforePresentation();
    void configuresInitialWindowDecorationBeforePresentation_data();
    void configuresInitialWindowDecorationBeforePresentation();
    void togglesWindowDecorationsPerWindowAcrossReload();
    void defersNonWindowedSessionUntilExposedGeometry_data();
    void defersNonWindowedSessionUntilExposedGeometry();
    void windowStateReloadAffectsOnlyFutureWindows();
    void configuresInitialWindowGeometryBeforePresentation_data();
    void configuresInitialWindowGeometryBeforePresentation();
    void windowGeometryReloadAffectsOnlyFutureWindows();
    void integratesQuickTerminalLifecycleAndLiveShellReload();
    void quickTerminalAutohideRequiresActivationAndReloadsLive();
    void retiringQuickTerminalIsNeverReshown();
    void routesWindowUiActionsAndNotificationPolicy();
    void routesCrashActionsByStablePaneAndTypedTarget();
    void configurationFailuresReachExistingAndFutureWindows();
    void configurationDiagnosticsDialogIsScrollableAndExplicit();
    void toastPresentationExpiresQueuedItems();
    void chromeToolButtonUsesWindowTextForFlatIcon();
    void chromeToolButtonSuppressesActionTextMnemonic();
    void remoteNewWindowOverridesOnlyItsFirstSurface();
    void remoteNewTabTargetsProcessSurfaceAndScopesOverrides();
    void paletteTargetsEveryLiveSurfaceByCompositeIdentity();
    void paletteTargetFocusMayDestroyApplicationController();
    void sharesOneKeybindProgramGenerationAcrossWindows();
    void globalBindingWaitsForPaneReloadTransaction();
    void terminalBarrierWaitsForConfigurationTransaction();
    void rootReleaseWaitsForNestedReloadTransaction();
    void windowCreationCatchesUpReloadFromFactory();
    void initialGeometryDestructionCannotLeaveHalfRegisteredWindow_data();
    void initialGeometryDestructionCannotLeaveHalfRegisteredWindow();
    void preservesCompositeSourceAndWindowInheritancePolicies();
    void residentProcessReloadsRecreatesAndQuitsWithZeroWindows();
    void configuredQuitWaitsForCompleteActionChain();
    void configuredCloseWindowQuitEscalatesBeforePublication();
    void configuredCloseWindowQuitUsesOpenConfirmationHost();
    void configuredApplicationCallbackWaitsForCompleteActionChain_data();
    void configuredApplicationCallbackWaitsForCompleteActionChain();
    void rootApplicationChainSurvivesSourceDestruction();
    void configuredNewWindowAndQuitPreserveOrder_data();
    void configuredNewWindowAndQuitPreserveOrder();
    void commandPrecedenceAndReloadUseFirstSessionLease();
    void suppressedStartupPreservesFirstSessionOptions();
    void failedLazyCreationDoesNotConsumeFirstSession();
    void terminalInitializationFailurePromotesNextSession();
    void waitingControllersCancelWithoutCreatingWorkers();
    void immediateTabWinsInitialSessionLease();
    void reverseExposureGrantsInitialSessionToFirstStarter();
    void ordinaryInheritanceDoesNotWaitForFirstSession();
    void deferredRequestsRemainOrderedAcrossStart();
    void approvedCloseUnmapsBeforeDestroyingWindow();
    void ordinaryCloseUsesOnlyTheFinalWindowForLifetimePolicy();
    void successfulReplacementCancelsDelayedQuit();
    void navigatesLiveWindowsInRegistrationOrder();
    void sourceLessActivationMatchesUpstreamInheritance();
    void sourceLessActivationCancelsQuitAndReportsFailure();
    void sourceLessActivationRequiresStartupDecision();
    void sourceLessActivationProjectsDesktopContext();
    void reentrantWindowCreationIsRejected();
    void controllerMayBeDestroyedDuringWindowCreation_data();
    void controllerMayBeDestroyedDuringWindowCreation();
    void showDestructionCannotLeaveHalfRegisteredWindow();
    void failedInitialPresentationPreservesFirstSession();
    void explicitQuitWaitsForWindowRetirement();
    void explicitQuitAggregatesEveryWindowIntoOneConfirmation();
    void reentrantQuitDuringClosePublicationDoesNotLoseApproval();
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
    // Mirror the process policy installed by main() before its first Quick
    // window. All injected factory windows below should therefore request an
    // alpha-capable native surface, irrespective of current terminal opacity.
    QQuickWindow::setDefaultAlphaBuffer(true);
    QVERIFY(QQuickWindow::hasDefaultAlphaBuffer());
    QVERIFY(QDir().mkpath(
        QDir::current().filePath(QStringLiteral("tmp"))));
}

void ApplicationControllerTest::
    consumesDeprecatedCloseAllWindowsWithoutMutation()
{
    WindowFactoryHarness emptyHarness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController empty(options, emptyHarness.factory(), false);
    QVERIFY(empty.dispatch(ApplicationAction::DeprecatedCloseAllWindows));
    QCOMPARE(empty.windowCount(), 0);
    QCOMPARE(emptyHarness.calls, 0);

    WindowFactoryHarness harness;
    ApplicationController controller(options, harness.factory(), false);
    const auto first = controller.createInitialWindow();
    QVERIFY(first.has_value());
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);

    const QVector<ApplicationWindow> before = controller.windows();
    QSignalSpy quitRequested(&controller,
                             &ApplicationController::quitRequested);
    QSignalSpy quitCommitted(&controller,
                             &ApplicationController::applicationQuitCommitted);
    QSignalSpy openConfig(&controller,
                          &ApplicationController::configOpenRequested);
    QSignalSpy reloadConfig(&controller,
                            &ApplicationController::configReloadRequested);

    QVERIFY(controller.dispatch(ApplicationAction::DeprecatedCloseAllWindows,
                                first->workspace,
                                activePaneId(first->workspace)));
    QCoreApplication::processEvents();
    QCOMPARE(controller.windowCount(), 2);
    const QVector<ApplicationWindow> after = controller.windows();
    QCOMPARE(after.size(), before.size());
    for (qsizetype index = 0; index < before.size(); ++index) {
        QCOMPARE(after[index].window, before[index].window);
        QCOMPARE(after[index].workspace, before[index].workspace);
    }
    QCOMPARE(quitRequested.count(), 0);
    QCOMPARE(quitCommitted.count(), 0);
    QCOMPARE(openConfig.count(), 0);
    QCOMPARE(reloadConfig.count(), 0);
    QVERIFY(!controller.lifetimeController()->hasRequestedQuit());
}

void ApplicationControllerTest::
    integratesQuickTerminalLifecycleAndLiveShellReload()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.environment = {
        {
            .key = QByteArrayLiteral("GHOSTTY_QUICK_TERMINAL"),
            .value = QByteArrayLiteral("stale-first"),
        },
        {
            .key = QByteArrayLiteral("UNCHANGED"),
            .value = QByteArrayLiteral("yes"),
        },
        {
            .key = QByteArrayLiteral("GHOSTTY_QUICK_TERMINAL"),
            .value = QByteArrayLiteral("stale-last"),
        },
    };
    options.applicationShell.commandPalette = {{
        .title = QStringLiteral("Initial command"),
        .actionKey = QStringLiteral("initial"),
        .action = QStringLiteral("new_tab"),
    }};
    options.quickTerminalLayerShell = {
        .layer = QuickTerminalLayer::Bottom,
        .layerNamespace = QStringLiteral("initial-quick-terminal"),
    };

    ApplicationController controller(options, harness.factory(), false);
    const auto ordinary = controller.createInitialWindow();
    QVERIFY(ordinary.has_value());
    QVERIFY(!ordinary->window->property("quickTerminal").toBool());
    QVERIFY(controller.dispatch(ApplicationAction::ToggleQuickTerminal));
    QCOMPARE(controller.windowCount(), 2);
    QCOMPARE(harness.calls, 2);

    const std::optional<ApplicationWindow> quick =
        quickTerminalWindow(controller);
    QVERIFY(quick.has_value());
    QVERIFY(quick->window != ordinary->window);
    QVERIFY(quick->window->isVisible());
    QVERIFY(quick->window->flags().testFlag(Qt::FramelessWindowHint));
    QCOMPARE(harness.sessionStartedAtPresentation.size(), 2);
    QVERIFY(!harness.sessionStartedAtPresentation.constLast());

    TerminalController *const terminal = onlyController(quick->workspace);
    QVERIFY(terminal != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(terminal->sessionStarted(), 1000);
    const TerminalEnvironment &environment = terminal->launchEnvironment();
    const auto isQuickTerminalEntry =
        [](const TerminalEnvironmentEntry &entry) {
            return entry.key == QByteArrayLiteral("GHOSTTY_QUICK_TERMINAL");
        };
    QCOMPARE(static_cast<int>(
                 std::ranges::count_if(environment, isQuickTerminalEntry)),
             1);
    const auto quickEnvironment =
        std::ranges::find_if(environment, isQuickTerminalEntry);
    QVERIFY(quickEnvironment != environment.cend());
    QCOMPARE(quickEnvironment->value, QByteArrayLiteral("1"));
    QVERIFY(std::ranges::any_of(
        environment, [](const TerminalEnvironmentEntry &entry) {
            return entry.key == QByteArrayLiteral("UNCHANGED")
                && entry.value == QByteArrayLiteral("yes");
        }));

    auto *const layer = LayerShellQt::Window::get(quick->window);
    QVERIFY(layer != nullptr);
    QCOMPARE(layer->anchors(),
             LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop));
    QCOMPARE(layer->layer(), LayerShellQt::Window::LayerBottom);
    QCOMPARE(layer->scope(), QStringLiteral("initial-quick-terminal"));
    WindowUiController *const ordinaryUi = windowUiController(ordinary->window);
    WindowUiController *const quickUi = windowUiController(quick->window);
    QVERIFY(ordinaryUi != nullptr);
    QVERIFY(quickUi != nullptr);
    QCOMPARE(ordinaryUi->commandPaletteModel()->count(), 1);
    QCOMPARE(quickUi->commandPaletteModel()->count(), 1);

    const GhosttyKeybindProgram generation = controller.keybindProgram();
    LaunchOptions reloaded = options;
    reloaded.applicationShell.quickTerminal.position =
        QuickTerminalPosition::Bottom;
    reloaded.applicationShell.quickTerminal.size.primary =
        QuickTerminalPercentage{37.5F};
    reloaded.applicationShell.quickTerminal.keyboardInteractivity =
        QuickTerminalKeyboardInteractivity::None;
    reloaded.backgroundBlur = 20;
    reloaded.quickTerminalLayerShell = {
        .layer = QuickTerminalLayer::Overlay,
        .layerNamespace = QStringLiteral("recreated-quick-terminal"),
    };
    reloaded.applicationShell.commandPalette = {{
        .title = QStringLiteral("Reloaded command"),
        .actionKey = QStringLiteral("reloaded"),
        .action = QStringLiteral("toggle_tab_overview"),
    }};
    controller.applyLaunchOptions(reloaded);

    QVERIFY(controller.keybindProgram().isSameGeneration(generation));
    QCOMPARE(controller.windowCount(), 2);
    QCOMPARE(harness.calls, 2);
    QCOMPARE(layer->anchors(),
             LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorBottom));
    QCOMPARE(layer->keyboardInteractivity(),
             LayerShellQt::Window::KeyboardInteractivityNone);
    QCOMPARE(layer->layer(), LayerShellQt::Window::LayerOverlay);
    QCOMPARE(layer->scope(), QStringLiteral("recreated-quick-terminal"));
    QVERIFY(!layer->activateOnShow());
    QCOMPARE(ordinaryUi->commandPaletteModel()->count(), 1);
    QCOMPARE(ordinaryUi->commandPaletteModel()
                 ->data(ordinaryUi->commandPaletteModel()->index(0, 0),
                        CommandPaletteModel::TitleRole)
                 .toString(),
             QStringLiteral("Reloaded command"));
    QCOMPARE(quickUi->commandPaletteModel()->count(), 1);

    QVERIFY(controller.dispatch(ApplicationAction::ToggleQuickTerminal));
    QVERIFY(!quick->window->isVisible());
    QCOMPARE(controller.windowCount(), 2);
    QVERIFY(controller.dispatch(ApplicationAction::ToggleQuickTerminal));
    QVERIFY(quick->window->isVisible());
    QCOMPARE(quickTerminalWindow(controller)->window, quick->window);
    QCOMPARE(harness.calls, 2);
    QVERIFY(controller.dispatch(ApplicationAction::ToggleQuickTerminal));
    QVERIFY(!quick->window->isVisible());
}

void ApplicationControllerTest::
    quickTerminalAutohideRequiresActivationAndReloadsLive()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.applicationShell.quickTerminal.autohide = true;
    options.applicationShell.quickTerminal.keyboardInteractivity =
        QuickTerminalKeyboardInteractivity::None;

    ApplicationController controller(options, harness.factory(), false);
    const auto ordinary = controller.createInitialWindow();
    QVERIFY(ordinary.has_value());
    QVERIFY(controller.dispatch(ApplicationAction::ToggleQuickTerminal));
    const std::optional<ApplicationWindow> quick =
        quickTerminalWindow(controller);
    QVERIFY(quick.has_value());

    // A never-focusable surface cannot acknowledge activation. It must remain
    // visible rather than interpreting initial compositor focus settling as
    // an autohide transition.
    QTest::qWait(80);
    QVERIFY(quick->window->isVisible());

    options.applicationShell.quickTerminal.keyboardInteractivity =
        QuickTerminalKeyboardInteractivity::OnDemand;
    controller.applyLaunchOptions(options);
    quick->window->requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(quick->window->isActive(), 1000);
    ordinary->window->requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(ordinary->window->isActive(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!quick->window->isVisible(), 1000);

    QVERIFY(controller.dispatch(ApplicationAction::ToggleQuickTerminal));
    QTRY_VERIFY_WITH_TIMEOUT(quick->window->isActive(), 1000);
    ordinary->window->requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(ordinary->window->isActive(), 1000);

    LaunchOptions disabled = options;
    disabled.applicationShell.quickTerminal.autohide = false;
    controller.applyLaunchOptions(disabled);
    QTest::qWait(80);
    QVERIFY(quick->window->isVisible());
    QCOMPARE(controller.windowCount(), 2);
    QCOMPARE(harness.calls, 2);

    // Re-enabling while the acknowledged surface remains inactive starts the
    // same settling timer; no show, pane, or PTY recreation is required.
    controller.applyLaunchOptions(options);
    QTRY_VERIFY_WITH_TIMEOUT(!quick->window->isVisible(), 1000);
    QCOMPARE(controller.windowCount(), 2);
    QCOMPARE(harness.calls, 2);
}

void ApplicationControllerTest::retiringQuickTerminalIsNeverReshown()
{
    WindowFactoryHarness harness;
    ApplicationController controller(baseOptions(QDir::currentPath()),
                                     harness.factory(), false);
    QVERIFY(controller.createInitialWindow().has_value());
    QVERIFY(controller.dispatch(ApplicationAction::ToggleQuickTerminal));
    const std::optional<ApplicationWindow> quick =
        quickTerminalWindow(controller);
    QVERIFY(quick.has_value());
    QCOMPARE(harness.calls, 2);

    const QPointer<QQuickWindow> retiringWindow(quick->window);
    bool toggleAcceptedDuringUnmap = false;
    bool retiringWindowStayedHidden = false;
    connect(
        quick->window, &QWindow::visibleChanged, quick->window,
        [&controller, retiringWindow, &toggleAcceptedDuringUnmap,
         &retiringWindowStayedHidden](bool visible) {
            if (visible) return;
            toggleAcceptedDuringUnmap =
                controller.dispatch(ApplicationAction::ToggleQuickTerminal);
            retiringWindowStayedHidden =
                retiringWindow != nullptr && !retiringWindow->isVisible();
        },
        Qt::SingleShotConnection);

    closeWorkspace(quick->workspace);

    QTRY_VERIFY_WITH_TIMEOUT(toggleAcceptedDuringUnmap, 1000);
    QVERIFY(retiringWindowStayedHidden);
    QCOMPARE(harness.calls, 3);
    QTRY_VERIFY_WITH_TIMEOUT(retiringWindow.isNull(), 1000);
    const std::optional<ApplicationWindow> replacement =
        quickTerminalWindow(controller);
    QVERIFY(replacement.has_value());
    QVERIFY(replacement->window != retiringWindow);
}

void ApplicationControllerTest::routesWindowUiActionsAndNotificationPolicy()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.configuredTitle = QStringLiteral("notice title");
    options.applicationShell.commandPalette = {
        {
            .title = QStringLiteral("Create tab"),
            .description = QStringLiteral("Create another terminal tab"),
            .actionKey = QStringLiteral("new_tab"),
            .action = QStringLiteral("new_tab"),
        },
        {
            .title = QStringLiteral("Unsupported"),
            .actionKey = QStringLiteral("undo"),
            .action = QStringLiteral("undo"),
        },
    };

    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    WindowUiController *const ui = windowUiController(initial->window);
    QVERIFY(ui != nullptr);
    QCOMPARE(ui->commandPaletteModel()->count(), 1);
    TerminalPane *const firstPane = onlyPane(initial->workspace);
    QVERIFY(firstPane != nullptr);

    QVERIFY(initial->workspace->executeActiveConfiguredAction(
        QStringLiteral("toggle_command_palette")));
    QCOMPARE(ui->modal(), WindowUiController::Modal::CommandPalette);
    QVERIFY(initial->workspace->executeActiveConfiguredAction(
        QStringLiteral("toggle_tab_overview")));
    QCOMPARE(ui->modal(), WindowUiController::Modal::TabOverview);
    ui->closeModal();

    QVERIFY(!firstPane->inspectorVisible());
    QVERIFY(initial->workspace->executeActiveConfiguredAction(
        QStringLiteral("inspector:show")));
    QVERIFY(firstPane->inspectorVisible());
    TerminalInspectorModel *const firstInspector = firstPane->inspectorModel();
    QVERIFY(firstInspector != nullptr);
    QVERIFY(initial->workspace->executeActiveConfiguredAction(
        QStringLiteral("inspector:show")));
    QCOMPARE(firstPane->inspectorModel(), firstInspector);
    QVERIFY(initial->workspace->executeActiveConfiguredAction(
        QStringLiteral("inspector:toggle")));
    QVERIFY(!firstPane->inspectorVisible());
    QVERIFY(initial->workspace->executeActiveConfiguredAction(
        QStringLiteral("inspector:hide")));
    QVERIFY(!firstPane->inspectorVisible());

    // Frontend requests retain a stable originating PaneId. A stale identity
    // must not open a window modal after its pane has disappeared.
    Q_EMIT initial->workspace->frontendActionRequested({
        .action = WorkspaceFrontendActions::ToggleCommandPalette{},
        .context = {.paneId = PaneId(std::numeric_limits<quint64>::max())},
    });
    QCOMPARE(ui->modal(), WindowUiController::Modal::None);
    Q_EMIT initial->workspace->frontendActionRequested({
        .action =
            WorkspaceFrontendActions::Inspector{
                .mode = WorkspaceFrontendActions::InspectorMode::Show,
            },
        .context = {.paneId = PaneId(std::numeric_limits<quint64>::max())},
    });
    QVERIFY(!firstPane->inspectorVisible());

    const TabId firstTab = initial->workspace->tabModel()->idAt(0);
    QVERIFY(firstTab.isValid());
    initial->workspace->newTab();
    QCOMPARE(initial->workspace->tabCount(), 2);
    QVERIFY(initial->workspace->currentIndex() != 0);
    QVERIFY(initial->workspace->activateTabByStableId(firstTab.value()));
    QCOMPARE(initial->workspace->currentIndex(), 0);
    QVERIFY(!initial->workspace->activateTabByStableId(0));

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const QString previousClipboard = clipboard->text();
    const auto restoreClipboard = qScopeGuard([clipboard, previousClipboard] {
        clipboard->setText(previousClipboard);
    });

    QVERIFY(initial->workspace->executeActiveConfiguredAction(
        QStringLiteral("copy_title_to_clipboard")));
    QCOMPARE(clipboard->text(), QStringLiteral("notice title"));
    QCOMPARE(ui->toastMessage(), QStringLiteral("Copied to clipboard"));
    ui->clearToasts();

    LaunchOptions clipboardDisabled = options;
    clipboardDisabled.applicationShell.notifications.clipboardCopy = false;
    controller.applyLaunchOptions(clipboardDisabled);
    QVERIFY(initial->workspace->executeActiveConfiguredAction(
        QStringLiteral("copy_title_to_clipboard")));
    QVERIFY(!ui->toastVisible());
    controller.notifyConfigurationReloaded();
    QCOMPARE(ui->toastMessage(), QStringLiteral("Reloaded the configuration"));
    ui->clearToasts();

    LaunchOptions reloadDisabled = options;
    reloadDisabled.applicationShell.notifications.configReload = false;
    controller.applyLaunchOptions(reloadDisabled);
    controller.notifyConfigurationReloaded();
    QVERIFY(!ui->toastVisible());

    // Empty commits use distinct presentation text, while still following
    // the live clipboard-copy policy on the originating window only.
    Q_EMIT firstPane->standardClipboardCommitted(true);
    QCOMPARE(ui->toastMessage(), QStringLiteral("Cleared clipboard"));
}

void ApplicationControllerTest::routesCrashActionsByStablePaneAndTypedTarget()
{
    struct Invocation {
        QQuickWindow *window = nullptr;
        TerminalWorkspace *workspace = nullptr;
        PaneId paneId;
        WorkspaceFrontendActions::CrashTarget target =
            WorkspaceFrontendActions::CrashTarget::Main;
    };

    QVector<Invocation> invocations;
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(
        options, harness.factory(),
        [&invocations](QQuickWindow *window, TerminalWorkspace *workspace,
                       PaneId paneId,
                       WorkspaceFrontendActions::CrashTarget target) {
            invocations.append({window, workspace, paneId, target});
            return true;
        },
        false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());

    const QVector<WorkspaceSurfaceSnapshot> surfaces =
        initial->workspace->surfaceSnapshot();
    QCOMPARE(surfaces.size(), 1);
    const PaneId paneId = surfaces.constFirst().paneId;
    QVERIFY(paneId.isValid());

    const struct {
        const char *action;
        WorkspaceFrontendActions::CrashTarget target;
    } cases[] = {
        {"crash:main", WorkspaceFrontendActions::CrashTarget::Main},
        {"crash:io", WorkspaceFrontendActions::CrashTarget::Io},
        {"crash:render", WorkspaceFrontendActions::CrashTarget::Render},
    };
    for (const auto &testCase : cases) {
        QVERIFY(initial->workspace->executeActiveConfiguredAction(
            QString::fromLatin1(testCase.action)));
        QCOMPARE(invocations.constLast().window, initial->window);
        QCOMPARE(invocations.constLast().workspace, initial->workspace);
        QCOMPARE(invocations.constLast().paneId, paneId);
        QCOMPARE(invocations.constLast().target, testCase.target);
    }
    QCOMPARE(invocations.size(), 3);

    Q_EMIT initial->workspace->frontendActionRequested({
        .action =
            WorkspaceFrontendActions::Crash{
                .target = WorkspaceFrontendActions::CrashTarget::Io,
            },
        .context =
            {
                .paneId = PaneId(std::numeric_limits<quint64>::max()),
            },
    });
    QCOMPARE(invocations.size(), 3);
}

void ApplicationControllerTest::
    configurationFailuresReachExistingAndFutureWindows()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(options, harness.factory(), false);

    int ghosttyReloads = 0;
    int frontendReloads = 0;
    connect(&controller, &ApplicationController::configReloadRequested,
            &controller, [&ghosttyReloads] { ++ghosttyReloads; });
    connect(&controller, &ApplicationController::configReloadRequested,
            &controller, [&frontendReloads] { ++frontendReloads; });

    controller.reportConfigurationFailure(
        ApplicationController::ConfigurationSource::Ghostty,
        QStringLiteral("invalid shared option"));
    const QString ghosttyOnly =
        QStringLiteral("Ghostty configuration:\ninvalid shared option");
    QCOMPARE(controller.configurationDiagnosticsText(), ghosttyOnly);

    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    WindowUiController *const firstUi = windowUiController(initial->window);
    QVERIFY(firstUi != nullptr);
    QCOMPARE(firstUi->configurationDiagnosticsText(), ghosttyOnly);
    QVERIFY(firstUi->configurationDiagnosticsVisible());

    QVERIFY(firstUi->retryConfigurationDiagnostics());
    QCOMPARE(ghosttyReloads, 1);
    QCOMPARE(frontendReloads, 1);
    QVERIFY(firstUi->configurationDiagnosticsVisible());
    QCOMPARE(firstUi->configurationDiagnosticsText(), ghosttyOnly);

    firstUi->ignoreConfigurationDiagnostics();
    QVERIFY(!firstUi->configurationDiagnosticsVisible());
    controller.reportConfigurationFailure(
        ApplicationController::ConfigurationSource::Ghostty,
        QStringLiteral("invalid shared option"));
    QVERIFY(!firstUi->configurationDiagnosticsVisible());

    // The process retains the current failure even after one window dismisses
    // it, so a later window still presents that state.
    QVERIFY(controller.activateNoCommand());
    QCOMPARE(controller.windowCount(), 2);
    const QVector<ApplicationWindow> windows = controller.windows();
    WindowUiController *const secondUi =
        windowUiController(windows.constLast().window);
    QVERIFY(secondUi != nullptr);
    QCOMPARE(secondUi->configurationDiagnosticsText(), ghosttyOnly);
    QVERIFY(secondUi->configurationDiagnosticsVisible());

    controller.reportConfigurationFailure(
        ApplicationController::ConfigurationSource::Frontend,
        QStringLiteral("invalid frontend option"));
    const QString combined = QStringLiteral(
        "Ghostty configuration:\ninvalid shared option\n\n"
        "ghostty-qt frontend configuration:\ninvalid frontend option");
    QCOMPARE(controller.configurationDiagnosticsText(), combined);
    QCOMPARE(firstUi->configurationDiagnosticsText(), combined);
    QCOMPARE(secondUi->configurationDiagnosticsText(), combined);
    QVERIFY(firstUi->configurationDiagnosticsVisible());
    QVERIFY(secondUi->configurationDiagnosticsVisible());

    controller.clearConfigurationFailure(
        ApplicationController::ConfigurationSource::Ghostty);
    const QString frontendOnly = QStringLiteral(
        "ghostty-qt frontend configuration:\ninvalid frontend option");
    QCOMPARE(firstUi->configurationDiagnosticsText(), frontendOnly);
    QCOMPARE(secondUi->configurationDiagnosticsText(), frontendOnly);
    QVERIFY(firstUi->configurationDiagnosticsVisible());
    QVERIFY(secondUi->configurationDiagnosticsVisible());

    controller.clearConfigurationFailure(
        ApplicationController::ConfigurationSource::Frontend);
    QVERIFY(controller.configurationDiagnosticsText().isEmpty());
    QVERIFY(firstUi->configurationDiagnosticsText().isEmpty());
    QVERIFY(secondUi->configurationDiagnosticsText().isEmpty());
    QVERIFY(!firstUi->configurationDiagnosticsVisible());
    QVERIFY(!secondUi->configurationDiagnosticsVisible());

    controller.reportConfigurationFailure(
        ApplicationController::ConfigurationSource::Frontend,
        QStringLiteral("invalid frontend option"));
    QVERIFY(firstUi->configurationDiagnosticsVisible());
    QVERIFY(secondUi->configurationDiagnosticsVisible());
}

void ApplicationControllerTest::
    configurationDiagnosticsDialogIsScrollableAndExplicit()
{
    WindowUiController ui;
    int retries = 0;
    ui.setConfigurationRetryCallback([&retries] { ++retries; });
    QStringList lines;
    for (int index = 0; index < 80; ++index) {
        lines.append(QStringLiteral("invalid option %1").arg(index));
    }
    const QString diagnostics =
        QStringLiteral("Ghostty configuration:\n") + lines.join(u'\n');
    ui.setConfigurationDiagnostics(diagnostics);

    const QString dialogPath =
        QFINDTESTDATA("../qml/ConfigDiagnosticsDialog.qml");
    QVERIFY(!dialogPath.isEmpty());
    const QFileInfo dialogInfo(dialogPath);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral("import QtQuick\n"
                          "import QtQuick.Controls\n"
                          "import \".\" as Local\n"
                          "ApplicationWindow {\n"
                          "    id: root\n"
                          "    required property var uiController\n"
                          "    width: 760\n"
                          "    height: 520\n"
                          "    visible: true\n"
                          "    Local.ConfigDiagnosticsDialog {\n"
                          "        uiController: root.uiController\n"
                          "    }\n"
                          "}\n"),
        QUrl::fromLocalFile(
            QDir(dialogInfo.absolutePath())
                .filePath(QStringLiteral("ConfigDiagnosticsHarness.qml"))));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.createWithInitialProperties({
        {QStringLiteral("uiController"),
         QVariant::fromValue(static_cast<QObject *>(&ui))},
    }));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));
    auto *const window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(window->isExposed(), 1000);

    QObject *const dialog = root->findChild<QObject *>(
        QStringLiteral("configurationDiagnosticsDialog"));
    auto *const textArea = root->findChild<QQuickItem *>(
        QStringLiteral("configurationDiagnosticsText"));
    auto *const scrollView = root->findChild<QQuickItem *>(
        QStringLiteral("configurationDiagnosticsScrollView"));
    auto *const retry = root->findChild<QQuickItem *>(
        QStringLiteral("configurationDiagnosticsRetry"));
    auto *const ignore = root->findChild<QQuickItem *>(
        QStringLiteral("configurationDiagnosticsIgnore"));
    QVERIFY(dialog != nullptr);
    QVERIFY(textArea != nullptr);
    QVERIFY(scrollView != nullptr);
    QVERIFY(retry != nullptr);
    QVERIFY(ignore != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(dialog->property("visible").toBool(), 1000);
    QCOMPARE(textArea->property("text").toString(), diagnostics);
    QVERIFY(textArea->property("readOnly").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(
        scrollView->property("contentHeight").toReal()
            > scrollView->property("availableHeight").toReal(),
        1000);
    QCOMPARE(retry->property("text").toString(), QStringLiteral("Retry"));
    QCOMPARE(ignore->property("text").toString(), QStringLiteral("Ignore"));

    const QPoint retryPosition =
        retry->mapToScene(QPointF(retry->width() / 2.0, retry->height() / 2.0))
            .toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, retryPosition);
    QCOMPARE(retries, 1);
    QVERIFY(dialog->property("visible").toBool());
    QCOMPARE(ui.configurationDiagnosticsText(), diagnostics);

    const QPoint ignorePosition =
        ignore
            ->mapToScene(QPointF(ignore->width() / 2.0, ignore->height() / 2.0))
            .toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, ignorePosition);
    QTRY_VERIFY_WITH_TIMEOUT(!dialog->property("visible").toBool(), 1000);
    QVERIFY(!ui.configurationDiagnosticsVisible());
}

void ApplicationControllerTest::toastPresentationExpiresQueuedItems()
{
    WindowUiController ui;
    const QString toastPath = QFINDTESTDATA("../qml/AppToast.qml");
    QVERIFY(!toastPath.isEmpty());
    const QFileInfo toastInfo(toastPath);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral("import QtQuick\n"
                          "import \".\" as Local\n"
                          "Item {\n"
                          "    property var uiController: null\n"
                          "    width: 400\n"
                          "    height: 200\n"
                          "    Local.AppToast {\n"
                          "        uiController: parent.uiController\n"
                          "    }\n"
                          "}\n"),
        QUrl::fromLocalFile(
            QDir(toastInfo.absolutePath())
                .filePath(QStringLiteral("AppToastHarness.qml"))));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    auto *const toast = root->findChild<QQuickItem *>(
        QStringLiteral("applicationToast"));
    QVERIFY(toast != nullptr);
    QVERIFY(!toast->isVisible());
    QVERIFY(root->setProperty(
        "uiController", QVariant::fromValue(static_cast<QObject *>(&ui))));

    QSignalSpy toastChanged(&ui, &WindowUiController::toastChanged);
    ui.enqueueToast(QStringLiteral("first"),
                    std::chrono::milliseconds{100});
    ui.enqueueToast(QStringLiteral("second"),
                    std::chrono::milliseconds{100});
    QCOMPARE(ui.toastQueueDepth(), 2);
    QVERIFY(toast->isVisible());

    QTRY_COMPARE_WITH_TIMEOUT(toastChanged.count(), 2, 1000);
    QCOMPARE(ui.toastMessage(), QStringLiteral("second"));
    QVERIFY(toast->isVisible());

    QTRY_COMPARE_WITH_TIMEOUT(toastChanged.count(), 3, 1000);
    QCOMPARE(ui.toastQueueDepth(), 0);
    QVERIFY(!toast->isVisible());
}

void ApplicationControllerTest::chromeToolButtonUsesWindowTextForFlatIcon()
{
    const QString buttonPath =
        QFINDTESTDATA("../qml/ChromeToolButton.qml");
    QVERIFY(!buttonPath.isEmpty());
    const QFileInfo buttonInfo(buttonPath);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral("import QtQuick\n"
                          "import QtQuick.Controls\n"
                          "import \".\" as Local\n"
                          "ApplicationWindow {\n"
                          "    Local.ChromeToolButton {\n"
                          "        objectName: \"chromeToolButton\"\n"
                          "        accessibleName: \"Test action\"\n"
                          "        flat: true\n"
                          "        palette.windowText: \"#102030\"\n"
                          "        palette.buttonText: \"#f0e0d0\"\n"
                          "        icon.source: Qt.resolvedUrl(\n"
                          "            \"icons/tab-new.svg\")\n"
                          "    }\n"
                          "}\n"),
        QUrl::fromLocalFile(
            QDir(buttonInfo.absolutePath())
                .filePath(QStringLiteral("ChromeToolButtonHarness.qml"))));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QObject *const button = root->findChild<QObject *>(
        QStringLiteral("chromeToolButton"));
    QVERIFY(button != nullptr);
    QVERIFY(button->property("flat").toBool());
    const QColor iconColor =
        QQmlProperty(button, QStringLiteral("icon.color")).read().value<QColor>();
    QCOMPARE(iconColor, QColor(QStringLiteral("#102030")));
}

void ApplicationControllerTest::chromeToolButtonSuppressesActionTextMnemonic()
{
    const QString buttonPath =
        QFINDTESTDATA("../qml/ChromeToolButton.qml");
    QVERIFY(!buttonPath.isEmpty());
    const QFileInfo buttonInfo(buttonPath);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral("import QtQuick\n"
                          "import QtQuick.Controls\n"
                          "import \".\" as Local\n"
                          "ApplicationWindow {\n"
                          "    property bool actionEnabled: true\n"
                          "    Action {\n"
                          "        id: testAction\n"
                          "        objectName: \"testAction\"\n"
                          "        text: \"Split Down\"\n"
                          "        enabled: actionEnabled\n"
                          "        icon.source: Qt.resolvedUrl(\n"
                          "            \"icons/split-down.svg\")\n"
                          "    }\n"
                          "    Local.ChromeToolButton {\n"
                          "        objectName: \"chromeToolButton\"\n"
                          "        action: testAction\n"
                          "        accessibleName: testAction.text\n"
                          "    }\n"
                          "}\n"),
        QUrl::fromLocalFile(
            QDir(buttonInfo.absolutePath())
                .filePath(QStringLiteral("ChromeToolButtonActionHarness.qml"))));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QObject *const action =
        root->findChild<QObject *>(QStringLiteral("testAction"));
    QObject *const button = root->findChild<QObject *>(
        QStringLiteral("chromeToolButton"));
    QVERIFY(action != nullptr);
    QVERIFY(button != nullptr);
    QCOMPARE(button->property("action").value<QObject *>(), action);
    QCOMPARE(button->property("text").toString(), QString{});
    QCOMPARE(button->property("accessibleName").toString(),
             QStringLiteral("Split Down"));
    QAccessibleInterface *const accessible =
        QAccessible::queryAccessibleInterface(button);
    QVERIFY(accessible != nullptr);
    QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Split Down"));

    QVERIFY(root->setProperty("actionEnabled", false));
    QVERIFY(!button->property("enabled").toBool());
}

void ApplicationControllerTest::remoteNewWindowOverridesOnlyItsFirstSurface()
{
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/application-remote-window-XXXXXX")));
    QVERIFY(directory.isValid());

    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.program = {
        QStringLiteral("/bin/sleep"),
        QStringLiteral("30"),
    };
    ApplicationController controller(options, harness.factory(), false);
    QVERIFY(controller.startWithoutInitialWindow());

    const TerminalCommand remoteCommand = TerminalCommand::direct({
        QByteArrayLiteral("/bin/sh"),
        QByteArrayLiteral("-c"),
        QByteArrayLiteral("sleep 30"),
    });
    QVERIFY(controller.activateNewWindow({
        .command = remoteCommand,
        .workingDirectory = directory.path(),
        .titleOverride = QString{},
    }));
    QCOMPARE(controller.windowCount(), 1);
    const ApplicationWindow first = controller.windows().constFirst();
    TerminalPane *const firstPane = onlyPane(first.workspace);
    TerminalController *const firstTerminal = onlyController(first.workspace);
    QVERIFY(firstPane != nullptr);
    QVERIFY(firstTerminal != nullptr);
    QCOMPARE(firstTerminal->launchProgram(), QStringList{});
    QCOMPARE(firstTerminal->launchCommand(),
             std::optional<TerminalCommand>{remoteCommand});
    QVERIFY(!firstTerminal->launchHold());
    QVERIFY(firstTerminal->hasTitle());
    QCOMPARE(firstTerminal->title(), QStringLiteral("/bin/sh"));
    QCOMPARE(firstTerminal->launchWorkingDirectory(), directory.path());
    QVERIFY(!firstTerminal->launchInheritsWorkingDirectory());
    QCOMPARE(firstPane->surfaceTitleOverride(),
             std::optional<QString>{QString{}});
    QCOMPARE(firstPane->effectiveSurfaceTitle(),
             std::optional<QString>{QString{}});

    first.workspace->newTab();
    QTRY_COMPARE_WITH_TIMEOUT(first.workspace->tabCount(), 2, 1000);
    const QList<TerminalPane *> firstWindowPanes =
        first.workspace->findChildren<TerminalPane *>();
    QCOMPARE(firstWindowPanes.size(), 2);
    const auto ordinary =
        std::ranges::find_if(firstWindowPanes, [firstPane](TerminalPane *pane) {
            return pane != firstPane;
        });
    QVERIFY(ordinary != firstWindowPanes.cend());
    QCOMPARE((*ordinary)->surfaceTitleOverride(), std::nullopt);
    TerminalController *const ordinaryTerminal =
        (*ordinary)->findChild<TerminalController *>();
    QVERIFY(ordinaryTerminal != nullptr);
    QVERIFY(ordinaryTerminal->launchCommand() != remoteCommand);

    QVERIFY(controller.activateNewWindow({
        .command = remoteCommand,
        .workingDirectory = QStringLiteral("inherit"),
        .titleOverride = QStringLiteral("inherited"),
    }));
    QCOMPARE(controller.windowCount(), 2);
    TerminalController *const literalInherit =
        onlyController(controller.windows().constLast().workspace);
    QVERIFY(literalInherit != nullptr);
    QCOMPARE(literalInherit->launchWorkingDirectory(),
             QStringLiteral("inherit"));
    QVERIFY(!literalInherit->launchInheritsWorkingDirectory());
    QCOMPARE(literalInherit->launchCommand(),
             std::optional<TerminalCommand>{remoteCommand});

    QVERIFY(controller.activateNewWindow({
        .workingDirectory = QStringLiteral("home"),
    }));
    QCOMPARE(controller.windowCount(), 3);
    TerminalController *const literalHome =
        onlyController(controller.windows().constLast().workspace);
    QVERIFY(literalHome != nullptr);
    QCOMPARE(literalHome->launchWorkingDirectory(), QStringLiteral("home"));
    QVERIFY(!literalHome->launchInheritsWorkingDirectory());
}

void ApplicationControllerTest::
    remoteNewTabTargetsProcessSurfaceAndScopesOverrides()
{
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/application-remote-tab-XXXXXX")));
    QVERIFY(directory.isValid());

    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.program = {QStringLiteral("/bin/sleep"), QStringLiteral("30")};
    options.tabInheritWorkingDirectory = false;
    ApplicationController controller(options, harness.factory(), false);
    const auto first = controller.createInitialWindow();
    QVERIFY(first.has_value());
    QVERIFY(controller.activateNewWindow(GhosttyNewWindowTransportOverrides{}));
    QCOMPARE(controller.windowCount(), 2);
    const ApplicationWindow second = controller.windows().constLast();

    const QVector<WorkspaceSurfaceSnapshot> firstSurfaces =
        first->workspace->surfaceSnapshot();
    const QVector<WorkspaceSurfaceSnapshot> secondSurfaces =
        second.workspace->surfaceSnapshot();
    QCOMPARE(firstSurfaces.size(), 1);
    QCOMPARE(secondSurfaces.size(), 1);
    QVERIFY(firstSurfaces.constFirst().surfaceId.isValid());
    QVERIFY(secondSurfaces.constFirst().surfaceId.isValid());
    QVERIFY(firstSurfaces.constFirst().surfaceId
            != secondSurfaces.constFirst().surfaceId);

    const TerminalCommand remoteCommand = TerminalCommand::direct({
        QByteArrayLiteral("/bin/sh"), QByteArrayLiteral("-c"),
        QByteArrayLiteral("sleep 30"),
    });
    QVERIFY(controller.activateNewTab({
        .surfaceId = firstSurfaces.constFirst().surfaceId,
        .overrides = {
            .command = remoteCommand,
            .shellIntegration = GhosttyShellIntegrationMode::Fish,
            .workingDirectory = directory.path(),
            .titleOverride = QStringLiteral("remote tab"),
        },
    }));
    QCOMPARE(first->workspace->tabCount(), 2);
    QCOMPARE(second.workspace->tabCount(), 1);

    const QList<TerminalPane *> panes =
        first->workspace->findChildren<TerminalPane *>();
    const auto remote = std::ranges::find_if(
        panes, [](TerminalPane *pane) {
            return pane->surfaceTitleOverride()
                == std::optional<QString>{QStringLiteral("remote tab")};
        });
    QVERIFY(remote != panes.cend());
    TerminalController *const terminal =
        (*remote)->findChild<TerminalController *>();
    QVERIFY(terminal != nullptr);
    QCOMPARE(terminal->launchCommand(),
             std::optional<TerminalCommand>{remoteCommand});
    QCOMPARE(terminal->launchWorkingDirectory(), directory.path());
    QCOMPARE(terminal->launchShellIntegration(),
             GhosttyShellIntegrationMode::Fish);

    const QVector<WorkspaceSurfaceSnapshot> updatedSurfaces =
        first->workspace->surfaceSnapshot();
    const auto remoteSurface = std::ranges::find_if(
        updatedSurfaces, [](const WorkspaceSurfaceSnapshot &surface) {
            return surface.effectiveTitle
                == std::optional<QString>{QStringLiteral("remote tab")};
        });
    QVERIFY(remoteSurface != updatedSurfaces.cend());
    QVERIFY(controller.activateNewTab({
        .surfaceId = remoteSurface->surfaceId,
    }));
    QCOMPARE(first->workspace->tabCount(), 3);
    const QList<TerminalPane *> afterOrdinary =
        first->workspace->findChildren<TerminalPane *>();
    QCOMPARE(std::ranges::count_if(
                 afterOrdinary, [](TerminalPane *pane) {
                     return pane->surfaceTitleOverride().has_value();
                 }),
             1);

    WindowFactoryHarness residentHarness;
    ApplicationController resident(options, residentHarness.factory(), false);
    QVERIFY(resident.startWithoutInitialWindow());
    QVERIFY(resident.activateNewTab({
        .surfaceId = SurfaceId(0xdead),
        .overrides = {
            .titleOverride = QStringLiteral("fallback window"),
        },
    }));
    QCOMPARE(resident.windowCount(), 1);
    TerminalPane *const fallback = onlyPane(
        resident.windows().constFirst().workspace);
    QVERIFY(fallback != nullptr);
    QCOMPARE(fallback->surfaceTitleOverride(),
             std::optional<QString>{QStringLiteral("fallback window")});
}

void ApplicationControllerTest::
    paletteTargetsEveryLiveSurfaceByCompositeIdentity()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.program = {
        QStringLiteral("/bin/sleep"),
        QStringLiteral("30"),
    };
    options.applicationShell.commandPalette = {{
        .title = QStringLiteral("Create tab"),
        .description = QStringLiteral("Configured row"),
        .actionKey = QStringLiteral("new_tab"),
        .action = QStringLiteral("new_tab"),
    }};

    ApplicationController controller(options, harness.factory(), false);
    const auto first = controller.createInitialWindow();
    QVERIFY(first.has_value());
    TerminalPane *const firstPane = onlyPane(first->workspace);
    QVERIFY(firstPane != nullptr);
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_surface_title:Alpha")));
    first->workspace->newTab();
    QTRY_COMPARE_WITH_TIMEOUT(first->workspace->tabCount(), 2, 1000);
    first->workspace->setCurrentIndex(0);

    QVERIFY(controller.activateNewWindow(GhosttyNewWindowTransportOverrides{}));
    QCOMPARE(controller.windowCount(), 2);
    const ApplicationWindow second = controller.windows().constLast();
    TerminalPane *const secondFirstPane = onlyPane(second.workspace);
    QVERIFY(secondFirstPane != nullptr);
    QVERIFY(secondFirstPane->executeConfiguredAction(
        QStringLiteral("set_surface_title:Alpha")));
    second.workspace->newTab();
    QTRY_COMPARE_WITH_TIMEOUT(second.workspace->tabCount(), 2, 1000);
    const QString targetTitle = QStringLiteral("Target ") + QDir::currentPath();
    QVERIFY(second.workspace->executeActiveConfiguredAction(
        QStringLiteral("set_surface_title:") + targetTitle));
    second.workspace->setCurrentIndex(0);

    QVERIFY(controller.activateNewWindow({
        .titleOverride = QString{},
    }));
    const ApplicationWindow explicitEmpty = controller.windows().constLast();

    QVERIFY(controller.activateQuickTerminal());
    const std::optional<ApplicationWindow> quick =
        quickTerminalWindow(controller);
    QVERIFY(quick.has_value());
    QVERIFY(quick->workspace->executeActiveConfiguredAction(
        QStringLiteral("set_surface_title:Quick")));
    QVERIFY(controller.activateQuickTerminal());
    QVERIFY(!quick->window->isVisible());

    WindowUiController *const ui = windowUiController(first->window);
    QVERIFY(ui != nullptr);
    QCOMPARE(ui->commandPaletteModel()->count(), 1);
    ui->showCommandPalette();
    CommandPaletteModel *const model = ui->commandPaletteModel();
    QCOMPARE(model->count(), 7);

    int alphaRows = 0;
    for (int row = 0; row < model->count(); ++row) {
        if (model->data(model->index(row, 0), CommandPaletteModel::TitleRole)
                .toString()
            == QStringLiteral("Focus: Alpha")) {
            ++alphaRows;
            const auto command = model->commandAt(row);
            QVERIFY(command.has_value());
            QVERIFY(std::holds_alternative<SurfaceTarget>(*command));
        }
    }
    QCOMPARE(alphaRows, 2);

    const int untitledRow =
        paletteRowWithTitle(*model, QStringLiteral("Focus: Untitled"));
    QVERIFY(untitledRow >= 0);
    QCOMPARE(model
                 ->data(model->index(untitledRow, 0),
                        CommandPaletteModel::DescriptionRole)
                 .toString(),
             QDir::currentPath());
    const int emptyTitleRow =
        paletteRowWithTitle(*model, QStringLiteral("Focus: "));
    QVERIFY(emptyTitleRow >= 0);
    QCOMPARE(model
                 ->data(model->index(emptyTitleRow, 0),
                        CommandPaletteModel::DescriptionRole)
                 .toString(),
             QDir::currentPath());

    LaunchOptions reloaded = options;
    reloaded.applicationShell.commandPalette = {{
        .title = QStringLiteral("Reloaded command"),
        .description = QStringLiteral("Live configured row"),
        .actionKey = QStringLiteral("new_tab"),
        .action = QStringLiteral("new_tab"),
    }};
    controller.applyLaunchOptions(reloaded);
    QCOMPARE(model->count(), 7);
    QVERIFY(paletteRowWithTitle(*model, QStringLiteral("Reloaded command"))
            >= 0);
    QCOMPARE(paletteRowWithTitle(*model, QStringLiteral("Create tab")), -1);
    QVERIFY(paletteRowWithTitle(*model, QStringLiteral("Focus: Untitled"))
            >= 0);
    QVERIFY(paletteRowWithTitle(*model, QStringLiteral("Focus: ")) >= 0);

    const int targetRow =
        paletteRowWithTitle(*model, QStringLiteral("Focus: ") + targetTitle);
    QVERIFY(targetRow >= 0);
    QCOMPARE(model
                 ->data(model->index(targetRow, 0),
                        CommandPaletteModel::DescriptionRole)
                 .toString(),
             QString{});
    model->setSelectedIndex(targetRow);
    ui->closeModal();
    QVERIFY(ui->activateSelectedCommand());
    QCOMPARE(second.workspace->currentIndex(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(second.window->isActive(), 1000);

    ui->showCommandPalette();
    const int quickRow =
        paletteRowWithTitle(*model, QStringLiteral("Focus: Quick"));
    QVERIFY(quickRow >= 0);
    model->setSelectedIndex(quickRow);
    ui->closeModal();
    QVERIFY(ui->activateSelectedCommand());
    QVERIFY(quick->window->isVisible());

    // A captured target becomes inert when only that pane disappears. It must
    // not fall through to the still-live pane selected in the same window.
    second.workspace->newTab();
    QTRY_COMPARE_WITH_TIMEOUT(second.workspace->tabCount(), 3, 1000);
    QVERIFY(second.workspace->executeActiveConfiguredAction(
        QStringLiteral("set_surface_title:Ephemeral")));
    ui->showCommandPalette();
    const int ephemeralRow =
        paletteRowWithTitle(*model, QStringLiteral("Focus: Ephemeral"));
    QVERIFY(ephemeralRow >= 0);
    model->setSelectedIndex(ephemeralRow);
    ui->closeModal();
    second.workspace->closeActivePane();
    QTRY_COMPARE_WITH_TIMEOUT(second.workspace->tabCount(), 2, 2000);
    second.workspace->setCurrentIndex(0);
    QVERIFY(ui->activateSelectedCommand());
    QCOMPARE(second.workspace->currentIndex(), 0);

    // Capture a stable composite target, retire its whole window, then invoke
    // it. A stale PaneId must not fall through to another window that happens
    // to own the same workspace-local numeric ID.
    QVERIFY(controller.activateQuickTerminal());
    QVERIFY(!quick->window->isVisible());
    ui->showCommandPalette();
    const int staleRow =
        paletteRowWithTitle(*model, QStringLiteral("Focus: ") + targetTitle);
    QVERIFY(staleRow >= 0);
    model->setSelectedIndex(staleRow);
    ui->closeModal();
    QPointer<QQuickWindow> retired(second.window);
    delete second.window;
    QVERIFY(retired.isNull());
    QCOMPARE(controller.windowCount(), 3);
    QVERIFY(ui->activateSelectedCommand());
    QVERIFY(first->window->isVisible());
    QVERIFY(!quick->window->isVisible());
    QVERIFY(explicitEmpty.window->isVisible());
}

void ApplicationControllerTest::
    paletteTargetFocusMayDestroyApplicationController()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.program = {
        QStringLiteral("/bin/sleep"),
        QStringLiteral("30"),
    };

    auto controller = std::make_unique<ApplicationController>(
        options, harness.factory(), false);
    const auto source = controller->createInitialWindow();
    QVERIFY(source.has_value());
    QVERIFY(controller->activateNewWindow({
        .titleOverride = QStringLiteral("Keep controller"),
    }));
    const ApplicationWindow target = controller->windows().constLast();
    target.workspace->newTab();
    QTRY_COMPARE_WITH_TIMEOUT(target.workspace->tabCount(), 2, 1000);
    QVERIFY(target.workspace->executeActiveConfiguredAction(
        QStringLiteral("set_surface_title:Delete controller")));
    target.workspace->setCurrentIndex(0);
    WindowUiController *const ui = windowUiController(source->window);
    QVERIFY(ui != nullptr);

    ui->showCommandPalette();
    CommandPaletteModel *const model = ui->commandPaletteModel();
    const int targetRow =
        paletteRowWithTitle(*model, QStringLiteral("Focus: Delete controller"));
    QVERIFY(targetRow >= 0);
    model->setSelectedIndex(targetRow);
    ui->closeModal();

    QObject::connect(
        target.workspace, &TerminalWorkspace::currentIndexChanged,
        target.workspace, [&controller] { controller.reset(); },
        Qt::SingleShotConnection);
    QVERIFY(ui->activateSelectedCommand());
    QVERIFY(controller == nullptr);
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

void ApplicationControllerTest::
    configuresInitialWindowDecorationBeforePresentation_data()
{
    QTest::addColumn<int>("decoration");
    QTest::addColumn<bool>("frameless");

    QTest::newRow("auto") << static_cast<int>(WindowDecorationMode::Auto)
                          << false;
    QTest::newRow("client")
        << static_cast<int>(WindowDecorationMode::Client) << false;
    QTest::newRow("server")
        << static_cast<int>(WindowDecorationMode::Server) << false;
    QTest::newRow("none") << static_cast<int>(WindowDecorationMode::None)
                          << true;
}

void ApplicationControllerTest::
    configuresInitialWindowDecorationBeforePresentation()
{
    QFETCH(int, decoration);
    QFETCH(bool, frameless);

    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.windowDecoration = static_cast<WindowDecorationMode>(decoration);
    ApplicationController controller(options, harness.factory(), false);

    const auto created = controller.createInitialWindow();
    QVERIFY(created.has_value());
    QCOMPARE(harness.presentations.size(), 1);
    QCOMPARE(harness.presentations.constFirst().flags.testFlag(
                 Qt::FramelessWindowHint),
             frameless);
    QCOMPARE(created->window->flags().testFlag(Qt::FramelessWindowHint),
             frameless);
}

void ApplicationControllerTest::togglesWindowDecorationsPerWindowAcrossReload()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.windowDecoration = WindowDecorationMode::Auto;
    ApplicationController controller(options, harness.factory(), false);

    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QQuickWindow *const firstWindow = initial->window;
    TerminalWorkspace *const firstWorkspace = initial->workspace;
    TerminalPane *const firstPane = onlyPane(firstWorkspace);
    TerminalController *const firstController = onlyController(firstWorkspace);
    QVERIFY(firstPane != nullptr);
    QVERIFY(firstController != nullptr);
    QVERIFY(!firstWindow->flags().testFlag(Qt::FramelessWindowHint));

    // The projection must mutate only the frameless hint. Client size,
    // visibility/state, QML restore policy, and every terminal object remain
    // intact across the live transition.
    firstWindow->setFlag(Qt::WindowDoesNotAcceptFocus, true);
    const QSize initialSize = firstWindow->size();
    const QWindow::Visibility initialVisibility = firstWindow->visibility();
    const Qt::WindowStates initialStates = firstWindow->windowStates();
    const QVariant initialFullscreenRestore =
        firstWindow->property("visibilityBeforeFullscreen");
    QVERIFY(firstWorkspace->dispatchAction({
        WorkspaceAction::ToggleWindowDecorations,
        {},
    }));
    QVERIFY(firstWindow->flags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(firstWindow->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
    QCOMPARE(firstWindow->size(), initialSize);
    QCOMPARE(firstWindow->visibility(), initialVisibility);
    QCOMPARE(firstWindow->windowStates(), initialStates);
    QCOMPARE(firstWindow->property("visibilityBeforeFullscreen"),
             initialFullscreenRestore);
    QCOMPARE(onlyPane(firstWorkspace), firstPane);
    QCOMPARE(onlyController(firstWorkspace), firstController);

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    const ApplicationWindow second = controller.windows().constLast();
    QVERIFY(second.window != firstWindow);
    QVERIFY(!second.window->flags().testFlag(Qt::FramelessWindowHint));

    // The first window's None override masks reload; the ordinary second
    // window follows it. Clearing the first override reveals the latest
    // configured Server value, which Qt maps to an ordinary decorated host.
    LaunchOptions reloaded = options;
    reloaded.windowDecoration = WindowDecorationMode::Server;
    controller.applyLaunchOptions(reloaded);
    QVERIFY(firstWindow->flags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(!second.window->flags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(firstWorkspace->dispatchAction({
        WorkspaceAction::ToggleWindowDecorations,
        {},
    }));
    QVERIFY(!firstWindow->flags().testFlag(Qt::FramelessWindowHint));

    QVERIFY(second.workspace->dispatchAction({
        WorkspaceAction::ToggleWindowDecorations,
        {},
    }));
    QVERIFY(second.window->flags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(!firstWindow->flags().testFlag(Qt::FramelessWindowHint));

    // A non-overridden window tracks None live. An overridden window remains
    // frameless until its second toggle clears the override.
    reloaded.windowDecoration = WindowDecorationMode::None;
    controller.applyLaunchOptions(reloaded);
    QVERIFY(firstWindow->flags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(second.window->flags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(firstWorkspace->dispatchAction({
        WorkspaceAction::ToggleWindowDecorations,
        {},
    }));
    QVERIFY(!firstWindow->flags().testFlag(Qt::FramelessWindowHint));

    reloaded.windowDecoration = WindowDecorationMode::Client;
    controller.applyLaunchOptions(reloaded);
    QVERIFY(!firstWindow->flags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(second.window->flags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(firstWorkspace->dispatchAction({
        WorkspaceAction::ToggleWindowDecorations,
        {},
    }));
    QVERIFY(!firstWindow->flags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(second.workspace->dispatchAction({
        WorkspaceAction::ToggleWindowDecorations,
        {},
    }));
    QVERIFY(!second.window->flags().testFlag(Qt::FramelessWindowHint));

    // New windows never inherit another window's runtime override; they use
    // the newest configuration.
    reloaded.windowDecoration = WindowDecorationMode::None;
    controller.applyLaunchOptions(reloaded);
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 3, 1000);
    const ApplicationWindow third = controller.windows().constLast();
    QVERIFY(third.window->flags().testFlag(Qt::FramelessWindowHint));
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
        options.typography, created->window->devicePixelRatio());
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
    QVERIFY(initial->window->requestedFormat().hasAlpha());
    const WId initialNativeWindowId = initial->window->winId();
    QQuickItem *const initialContentItem = initial->window->contentItem();
    QCOMPARE(harness.presentations.size(), 1);
    QCOMPARE(presentationWindowStates(initial->window->windowStates()),
             Qt::WindowStates(Qt::WindowNoState));

    LaunchOptions maximized = options;
    maximized.maximize = true;
    controller.applyLaunchOptions(maximized);
    QCOMPARE(controller.windows().constFirst().window, initial->window);
    QCOMPARE(initial->window->winId(), initialNativeWindowId);
    QCOMPARE(initial->window->contentItem(), initialContentItem);
    QVERIFY(initial->window->requestedFormat().hasAlpha());
    QCOMPARE(presentationWindowStates(initial->window->windowStates()),
             Qt::WindowStates(Qt::WindowNoState));
    QCOMPARE(initial->window->property(
                 "visibilityBeforeFullscreen").toInt(),
             static_cast<int>(QWindow::Windowed));

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.presentations.size(), 2, 1000);
    const ApplicationWindow actionCreated = controller.windows().constLast();
    QVERIFY(actionCreated.window->requestedFormat().hasAlpha());
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
    QVERIFY(residentReplacement.window->requestedFormat().hasAlpha());
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
    QVERIFY(activated.window->requestedFormat().hasAlpha());
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
    options.typography = sampleTypography(fontSize);
    ApplicationController controller(options, harness.factory(), false);

    const auto created = controller.createInitialWindow();
    QVERIFY(created.has_value());
    QCOMPARE(harness.presentations.size(), 1);

    const TerminalCellMetrics metrics = terminalCellMetrics(
        options.typography, created->window->devicePixelRatio());
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
        terminalCellMetrics(
            options.typography, initial->window->devicePixelRatio()),
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
        terminalCellMetrics(
            reloaded.typography,
            controller.windows().constLast().window->devicePixelRatio()),
        reloaded.windowWidth, reloaded.windowHeight, 0.0, 0.0);
    QCOMPARE(controller.windows().constLast().window->size(), reloadedSize);

    QVERIFY(initialPane->executeConfiguredAction(
        QStringLiteral("set_font_size:18")));
    QVERIFY(initialPane->executeConfiguredAction(
        QStringLiteral("new_window")));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 3, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.presentations.size(), 3, 1000);
    const QSize inheritedFontSize = gridWindowSize(
        terminalCellMetrics(
            typographyWithPointSize(reloaded.typography, 18.0),
            controller.windows().constLast().window->devicePixelRatio()),
        reloaded.windowWidth, reloaded.windowHeight, 0.0, 0.0);
    const ApplicationWindow inherited = controller.windows().constLast();
    QVERIFY(inherited.workspace->effectiveLaunchOptions().typography
            == typographyWithPointSize(reloaded.typography, 18.0));
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
            terminalCellMetrics(
                activated.typography,
                controller.windows().constFirst().window
                    ->devicePixelRatio()),
            activated.windowWidth, activated.windowHeight, 0.0, 0.0));
}

void ApplicationControllerTest::
sharesOneKeybindProgramGenerationAcrossWindows()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+k=ignore"),
    });
    ApplicationController controller(options, harness.factory(), false);

    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    initial->workspace->newTab();
    QCOMPARE(initial->workspace->tabCount(), 2);

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);

    const auto allWindowsShare = [&controller](
                                     const GhosttyKeybindProgram &program) {
        for (const ApplicationWindow &window : controller.windows()) {
            if (window.workspace == nullptr
                || !window.workspace->keybindProgram().isSameGeneration(
                    program)) {
                return false;
            }
            const QList<TerminalPane *> panes =
                window.workspace->findChildren<TerminalPane *>();
            if (panes.isEmpty()) return false;
            if (!std::ranges::all_of(
                    panes, [&program](const TerminalPane *pane) {
                        return pane->keybindProgram().isSameGeneration(
                            program);
                    })) {
                return false;
            }
        }
        return true;
    };

    const GhosttyKeybindProgram initialProgram =
        controller.keybindProgram();
    QVERIFY(allWindowsShare(initialProgram));

    LaunchOptions reloaded = options;
    reloaded.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+j=new_tab"),
    });
    controller.applyLaunchOptions(reloaded);

    const GhosttyKeybindProgram reloadedProgram =
        controller.keybindProgram();
    QVERIFY(!reloadedProgram.isSameGeneration(initialProgram));
    QVERIFY(allWindowsShare(reloadedProgram));

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 3, 1000);
    QVERIFY(controller.keybindProgram().isSameGeneration(reloadedProgram));
    QVERIFY(allWindowsShare(reloadedProgram));
}

void ApplicationControllerTest::globalBindingWaitsForPaneReloadTransaction()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.mouseReporting = false;
    options.keybindSource =
        GhosttyKeybindSource::structured(GhosttyKeybindConfig{});
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    const QVector<ApplicationWindow> windows = controller.windows();
    QCOMPARE(windows.size(), 2);

    TerminalPane *const firstPane = onlyPane(windows.at(0).workspace);
    TerminalController *const firstTerminal =
        onlyController(windows.at(0).workspace);
    TerminalPane *const laterPane = onlyPane(windows.at(1).workspace);
    TerminalController *const laterTerminal =
        onlyController(windows.at(1).workspace);
    QVERIFY(firstPane != nullptr);
    QVERIFY(firstTerminal != nullptr);
    QVERIFY(laterPane != nullptr);
    QVERIFY(laterTerminal != nullptr);
    QVERIFY(!firstTerminal->mouseReportingEnabled());
    QVERIFY(!laterTerminal->mouseReportingEnabled());
    QSignalSpy laterForwarded(
        laterTerminal, &TerminalController::keyRequested);

    GhosttyKeybindConfig reloadedConfig;
    reloadedConfig.root = {
        GhosttyKeybindDefinition{
            .sequence = {
                GhosttyKeybindTrigger{
                    .kind = GhosttyKeybindKeyKind::Unicode,
                    .unicodeCodepoint = 'g',
                },
            },
            .actions = {QStringLiteral("toggle_mouse_reporting")},
            .flags = GhosttyKeybindFlags{.global = true},
        },
    };
    LaunchOptions reloaded = options;
    reloaded.mouseReporting = true;
    reloaded.selectionClipboard.trimTrailingSpaces = false;
    reloaded.keybindSource =
        GhosttyKeybindSource::structured(std::move(reloadedConfig));

    bool injected = false;
    connect(firstTerminal, &TerminalController::runtimeOptionsRequested,
            firstPane, [&] {
                if (injected) return;
                injected = true;
                // The second workspace has not entered its pane-local reload
                // transaction yet. Only the process transaction can keep
                // this B root match from running over its A pane state.
                QKeyEvent press(
                    QEvent::KeyPress, Qt::Key_G, Qt::NoModifier,
                    QStringLiteral("g"));
                QCoreApplication::sendEvent(laterPane, &press);
                QKeyEvent release(
                    QEvent::KeyRelease, Qt::Key_G, Qt::NoModifier);
                QCoreApplication::sendEvent(laterPane, &release);
            });

    controller.applyLaunchOptions(reloaded);

    QVERIFY(injected);
    QVERIFY(firstPane->keybindProgram().isSameGeneration(
        controller.keybindProgram()));
    QVERIFY(laterPane->keybindProgram().isSameGeneration(
        controller.keybindProgram()));
    // Both configured true policies commit first; the deferred B binding then
    // toggles both once. Immediate mixed-generation fanout would leave the
    // later pane true when its reload subsequently overwrote the toggle.
    QVERIFY(!firstTerminal->mouseReportingEnabled());
    QVERIFY(!laterTerminal->mouseReportingEnabled());
    QCOMPARE(laterForwarded.count(), 0);
}

void ApplicationControllerTest::
    terminalBarrierWaitsForConfigurationTransaction()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    // This barrier test must remain within one live terminal-action epoch;
    // prompt pidfd delivery must not let the fixture's usual /bin/true child
    // expire the retained result while processEvents() drains the reload.
    options.program = {
        QStringLiteral("/bin/sleep"),
        QStringLiteral("30"),
    };
    GhosttyKeybindConfig config;
    config.root = {
        GhosttyKeybindDefinition{
            .sequence = {
                GhosttyKeybindTrigger{
                    .kind = GhosttyKeybindKeyKind::Unicode,
                    .unicodeCodepoint = 'g',
                    .modifiers = GhosttyKeybindCtrl,
                },
            },
            .actions = {
                QStringLiteral("write_screen_file:open"),
                QStringLiteral("increase_font_size:1"),
            },
            .flags = GhosttyKeybindFlags{.global = true},
        },
    };
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));

    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    TerminalPane *const pane = onlyPane(initial->workspace);
    TerminalController *const terminal =
        onlyController(initial->workspace);
    QVERIFY(pane != nullptr);
    QVERIFY(terminal != nullptr);

    quint64 requestId = 0;
    connect(
        terminal,
        &TerminalController::writeTerminalFileRequested,
        pane,
        [&requestId](
            quint64 emittedRequestId,
            const TerminalWriteFileAction &) {
            requestId = emittedRequestId;
        });
    bool insideConfigurationFanout = false;
    bool openedDuringConfigurationFanout = false;
    int openAttempts = 0;
    pane->setUrlOpener(
        [&](const QUrl &) {
            ++openAttempts;
            openedDuringConfigurationFanout |=
                insideConfigurationFanout;
            return true;
        });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier,
        QString(QChar(0x07)));
    QCoreApplication::sendEvent(pane, &press);
    QVERIFY(requestId != 0);
    QKeyEvent release(
        QEvent::KeyRelease, Qt::Key_G, Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &release);

    LaunchOptions reloaded = options;
    reloaded.typography.pointSize =
        options.typography.pointSize + 5.0;
    reloaded.selectionClipboard.trimTrailingSpaces =
        !options.selectionClipboard.trimTrailingSpaces;
    bool injected = false;
    connect(
        terminal, &TerminalController::runtimeOptionsRequested,
        pane, [&] {
            if (injected) return;
            injected = true;
            insideConfigurationFanout = true;
            Q_EMIT terminal->terminalActionReady({
                .requestId = requestId,
                .outcome = TerminalActionOutcome::Success,
                .effect = TerminalActionEffect::OpenFile,
                .performed = true,
                .payload =
                    QStringLiteral("/tmp/config-barrier-result.txt"),
                .clipboardDestination =
                    TerminalClipboardDestination::Standard,
            });
            QCoreApplication::processEvents();
            insideConfigurationFanout = false;
        });

    controller.applyLaunchOptions(reloaded);

    QVERIFY(injected);
    QVERIFY(!openedDuringConfigurationFanout);
    QCOMPARE(openAttempts, 1);
    QCOMPARE(
        pane->fontPointSize(),
        reloaded.typography.pointSize + 1.0);
}

void ApplicationControllerTest::rootReleaseWaitsForNestedReloadTransaction()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    GhosttyKeybindConfig config;
    config.root = {
        GhosttyKeybindDefinition{
            .sequence = {
                GhosttyKeybindTrigger{
                    .kind = GhosttyKeybindKeyKind::Unicode,
                    .unicodeCodepoint = 'k',
                    .modifiers = GhosttyKeybindCtrl,
                },
            },
            .actions = {QStringLiteral("toggle_readonly")},
            .flags = GhosttyKeybindFlags{.global = true},
        },
    };
    options.keybindSource =
        GhosttyKeybindSource::structured(config);

    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    TerminalPane *const pane = onlyPane(initial->workspace);
    TerminalController *const terminal = onlyController(initial->workspace);
    QVERIFY(pane != nullptr);
    QVERIFY(terminal != nullptr);
    QSignalSpy forwarded(terminal, &TerminalController::keyRequested);

    LaunchOptions reloaded = options;
    reloaded.selectionClipboard.trimTrailingSpaces = false;
    bool reloadedFromAction = false;
    bool releaseInjected = false;
    connect(terminal, &TerminalController::runtimeOptionsRequested,
            pane, [&] {
                if (releaseInjected) return;
                releaseInjected = true;
                QKeyEvent release(
                    QEvent::KeyRelease, Qt::Key_K,
                    Qt::ControlModifier);
                QCoreApplication::sendEvent(pane, &release);
            });
    connect(terminal, &TerminalController::readOnlyChanged,
            pane, [&](bool readOnly) {
                if (!readOnly || reloadedFromAction) return;
                reloadedFromAction = true;
                controller.applyLaunchOptions(reloaded);
            });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_K, Qt::ControlModifier,
        QString(QChar(0x0b)));
    QCoreApplication::sendEvent(pane, &press);

    QVERIFY(reloadedFromAction);
    QVERIFY(releaseInjected);
    QVERIFY(terminal->readOnly());
    // The nested reload cannot drain the matching release before the outer
    // root press records its consumed identity.
    QCOMPARE(forwarded.count(), 0);

    QKeyEvent laterRelease(
        QEvent::KeyRelease, Qt::Key_K, Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &laterRelease);
    // Draining the first release also removed the identity; no stale marker
    // may swallow a later unmatched release.
    QCOMPARE(forwarded.count(), 1);
}

void ApplicationControllerTest::windowCreationCatchesUpReloadFromFactory()
{
    WindowFactoryHarness harness;
    LaunchOptions initial = baseOptions(QDir::currentPath());
    initial.typography = sampleTypography(11.0);
    initial.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+i=ignore"),
    });
    LaunchOptions reloaded = initial;
    reloaded.typography = sampleTypography(18.0);
    reloaded.linkUrl = false;
    reloaded.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+r=new_tab"),
    });

    ApplicationController *application = nullptr;
    ApplicationController::WindowFactory baseFactory = harness.factory();
    ApplicationController::WindowFactory factory =
        [&, baseFactory = std::move(baseFactory)]() mutable
        -> std::expected<ApplicationWindow, QString> {
            application->applyLaunchOptions(reloaded);
            return baseFactory();
        };
    ApplicationController controller(initial, std::move(factory), false);
    application = &controller;
    const GhosttyKeybindProgram initialProgram =
        controller.keybindProgram();

    const auto created = controller.createInitialWindow();
    QVERIFY(created.has_value());
    const GhosttyKeybindProgram currentProgram =
        controller.keybindProgram();
    QVERIFY(!currentProgram.isSameGeneration(initialProgram));
    QVERIFY(created->workspace->keybindProgram().isSameGeneration(
        currentProgram));
    QVERIFY(created->workspace->effectiveLaunchOptions()
            == withoutInitialCommand(reloaded));

    TerminalPane *const pane = onlyPane(created->workspace);
    QVERIFY(pane != nullptr);
    QVERIFY(pane->keybindProgram().isSameGeneration(currentProgram));
    QCOMPARE(pane->fontPointSize(), reloaded.typography.pointSize);
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
    options.typography = sampleTypography(13.0);
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
    fallback.typography = sampleTypography(11.0);
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
    QVERIFY(secondWorkspace->effectiveLaunchOptions().typography
            == fallback.typography);
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
    QVERIFY(inheritedFirst->effectiveLaunchOptions().typography
            == typographyWithPointSize(inheriting.typography, 18.0));
    QVERIFY(inheritedFirst->effectiveLaunchOptions().program.isEmpty());
    QVERIFY(!inheritedFirst->effectiveLaunchOptions().hold);

    QVERIFY(secondPane->executeConfiguredAction(
        QStringLiteral("new_window")));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 4, 1000);
    TerminalWorkspace *const inheritedSecond =
        controller.windows().constLast().workspace;
    QCOMPARE(inheritedSecond->effectiveLaunchOptions().workingDirectory,
             secondDirectory);
    QVERIFY(inheritedSecond->effectiveLaunchOptions().typography
            == inheriting.typography);

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
    QVERIFY(staleSourceFallback->effectiveLaunchOptions().typography
            == inheriting.typography);
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
    QSignalSpy openConfigWindow(
        &controller,
        &ApplicationController::configOpenInNewWindowRequested);
    QVERIFY(controller.dispatch(ApplicationAction::OpenConfigNewWindow));
    QCOMPARE(openConfigWindow.count(), 1);

    QSignalSpy reload(&controller,
                      &ApplicationController::configReloadRequested);
    QVERIFY(controller.dispatch(ApplicationAction::ReloadConfig));
    QCOMPARE(reload.count(), 1);

    LaunchOptions reloaded = options;
    reloaded.typography = sampleTypography(17.0);
    controller.applyLaunchOptions(reloaded);
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    const ApplicationWindow replacement = controller.windows().constFirst();
    QVERIFY(replacement.workspace->effectiveLaunchOptions().typography
            == reloaded.typography);
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

void ApplicationControllerTest::configuredQuitWaitsForCompleteActionChain()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+k=quit"),
        QStringLiteral("chain=new_tab"),
    });
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QTRY_COMPARE_WITH_TIMEOUT(initial->workspace->tabCount(), 1, 1000);
    TerminalPane *const pane = onlyPane(initial->workspace);
    QVERIFY(pane != nullptr);

    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    const QPointer<TerminalWorkspace> guardedWorkspace(initial->workspace);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_K,
                    Qt::ControlModifier, QString(QChar(0x0b)));
    QCoreApplication::sendEvent(pane, &press);

    // The application owner commits quit on the next event-loop turn, after
    // the pane has executed the remaining surface action in the chain.
    QCOMPARE(initial->workspace->tabCount(), 2);
    QCOMPARE(quit.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QVERIFY(guardedWorkspace.isNull());
}

void ApplicationControllerTest::
configuredCloseWindowQuitEscalatesBeforePublication()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+k=close_window"),
        QStringLiteral("chain=quit"),
    });
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QTRY_COMPARE_WITH_TIMEOUT(initial->workspace->tabCount(), 1, 1000);
    TerminalPane *const pane = onlyPane(initial->workspace);
    QVERIFY(pane != nullptr);

    QStringList publicationOrder;
    connect(initial->workspace, &TerminalWorkspace::windowCloseApproved,
            &controller, [&publicationOrder] {
                publicationOrder.append(QStringLiteral("window"));
            });
    connect(initial->workspace, &TerminalWorkspace::applicationQuitApproved,
            &controller, [&publicationOrder] {
                publicationOrder.append(QStringLiteral("application"));
            });
    QSignalSpy committed(&controller,
                         &ApplicationController::applicationQuitCommitted);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    const QPointer<TerminalWorkspace> guardedWorkspace(initial->workspace);

    sendCtrlKPressAndRelease(pane);

    QVERIFY(guardedWorkspace != nullptr);
    QVERIFY(publicationOrder.isEmpty());
    QCOMPARE(committed.count(), 0);
    QCOMPARE(quit.count(), 0);

    const QStringList expected{
        QStringLiteral("window"), QStringLiteral("application")};
    QTRY_COMPARE_WITH_TIMEOUT(publicationOrder, expected, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
    QVERIFY(guardedWorkspace.isNull());
}

void ApplicationControllerTest::
configuredCloseWindowQuitUsesOpenConfirmationHost()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+k=close_window"),
        QStringLiteral("chain=quit"),
    });
    ApplicationController controller(options, harness.factory(), false);
    const auto protectedWindow = controller.createInitialWindow();
    QVERIFY(protectedWindow.has_value());
    QTRY_COMPARE_WITH_TIMEOUT(
        protectedWindow->workspace->tabCount(), 1, 1000);
    TerminalPane *const protectedPane =
        onlyPane(protectedWindow->workspace);
    QVERIFY(protectedPane != nullptr);
    QVERIFY(protectedPane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    const ApplicationWindow source = controller.windows().constLast();
    QCOMPARE(controller.activeWorkspace(), source.workspace);
    TerminalPane *const sourcePane = onlyPane(source.workspace);
    QVERIFY(sourcePane != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(!sourcePane->hasActiveProcess(), 3000);

    QSignalSpy protectedConfirmation(
        protectedWindow->workspace,
        &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy sourceConfirmation(
        source.workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy committed(&controller,
                         &ApplicationController::applicationQuitCommitted);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    const QPointer<TerminalWorkspace> guardedSource(source.workspace);

    sendCtrlKPressAndRelease(sourcePane);

    // The source close commits before queued quit routing. It can no longer
    // host the aggregate prompt, so the protected open workspace must do so.
    QTRY_COMPARE_WITH_TIMEOUT(protectedConfirmation.count(), 1, 1000);
    QCOMPARE(sourceConfirmation.count(), 0);
    QCOMPARE(protectedConfirmation.constFirst().at(1).toString(),
             QStringLiteral(
                 "A terminal window contains a read-only pane. Quit the application?"));
    QCOMPARE(committed.count(), 0);
    QCOMPARE(quit.count(), 0);

    protectedWindow->workspace->cancelClose(
        protectedConfirmation.constFirst().constFirst().toULongLong());
    QTRY_VERIFY_WITH_TIMEOUT(guardedSource.isNull(), 1000);
    QCOMPARE(controller.windowCount(), 1);
    QCOMPARE(committed.count(), 0);
    QCOMPARE(quit.count(), 0);
}

void ApplicationControllerTest::
configuredApplicationCallbackWaitsForCompleteActionChain_data()
{
    QTest::addColumn<QString>("action");

    QTest::newRow("open-config") << QStringLiteral("open_config");
    QTest::newRow("reload-config") << QStringLiteral("reload_config");
}

void ApplicationControllerTest::
configuredApplicationCallbackWaitsForCompleteActionChain()
{
    QFETCH(QString, action);

    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+k=%1").arg(action),
        QStringLiteral("chain=new_tab"),
    });
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QTRY_COMPARE_WITH_TIMEOUT(initial->workspace->tabCount(), 1, 1000);

    TerminalPane *const pane = onlyPane(initial->workspace);
    QVERIFY(pane != nullptr);
    TerminalController *const terminal = onlyController(initial->workspace);
    QVERIFY(terminal != nullptr);
    QSignalSpy forwarded(terminal, &TerminalController::keyRequested);
    const QPointer<TerminalWorkspace> guardedWorkspace(initial->workspace);
    const QPointer<TerminalPane> guardedPane(pane);
    int callbacks = 0;
    int observedTabCount = -1;
    const auto destroySource = [&] {
        ++callbacks;
        observedTabCount = guardedWorkspace != nullptr
            ? guardedWorkspace->tabCount() : -1;
        delete guardedWorkspace.data();
    };
    if (action == QStringLiteral("open_config")) {
        connect(&controller, &ApplicationController::configOpenRequested,
                &controller, destroySource);
    } else {
        connect(&controller, &ApplicationController::configReloadRequested,
                &controller, destroySource);
    }

    sendCtrlKPressAndRelease(pane);

    // The pane-local suffix completes before an externally reentrant
    // application callback crosses the process-owner boundary.
    QVERIFY(guardedWorkspace != nullptr);
    QVERIFY(guardedPane != nullptr);
    QCOMPARE(guardedWorkspace->tabCount(), 2);
    QCOMPARE(callbacks, 0);
    QCOMPARE(forwarded.count(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 1000);
    QCOMPARE(observedTabCount, 2);
    QVERIFY(guardedWorkspace.isNull());
    QVERIFY(guardedPane.isNull());
    QCOMPARE(controller.windowCount(), 0);
}

void ApplicationControllerTest::
rootApplicationChainSurvivesSourceDestruction()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+k=open_config"),
        QStringLiteral("chain=reload_config"),
    });
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QTRY_COMPARE_WITH_TIMEOUT(initial->workspace->tabCount(), 1, 1000);

    TerminalPane *const pane = onlyPane(initial->workspace);
    QVERIFY(pane != nullptr);
    TerminalController *const terminal = onlyController(initial->workspace);
    QVERIFY(terminal != nullptr);
    QSignalSpy forwarded(terminal, &TerminalController::keyRequested);
    const QPointer<TerminalWorkspace> guardedWorkspace(initial->workspace);
    QStringList callbacks;
    connect(&controller, &ApplicationController::configOpenRequested,
            &controller, [&] {
                callbacks.append(QStringLiteral("open"));
                delete guardedWorkspace.data();
            });
    connect(&controller, &ApplicationController::configReloadRequested,
            &controller, [&] {
                callbacks.append(QStringLiteral("reload"));
            });

    sendCtrlKPressAndRelease(pane);

    QVERIFY(callbacks.isEmpty());
    QVERIFY(guardedWorkspace != nullptr);
    QCOMPARE(forwarded.count(), 0);

    const QStringList expected{
        QStringLiteral("open"), QStringLiteral("reload")};
    QTRY_COMPARE_WITH_TIMEOUT(callbacks, expected, 1000);
    QVERIFY(guardedWorkspace.isNull());
    QCOMPARE(controller.windowCount(), 0);
}

void ApplicationControllerTest::
configuredNewWindowAndQuitPreserveOrder_data()
{
    QTest::addColumn<QStringList>("actions");
    QTest::addColumn<int>("expectedFactoryCalls");
    QTest::addColumn<QStringList>("expectedEvents");

    QTest::newRow("new-window-before-quit")
        << QStringList({QStringLiteral("new_window"),
                        QStringLiteral("quit")})
        << 2
        << QStringList({QStringLiteral("window"),
                        QStringLiteral("quit")});
    QTest::newRow("quit-before-new-window")
        << QStringList({QStringLiteral("quit"),
                        QStringLiteral("new_window")})
        << 1
        << QStringList({QStringLiteral("quit")});
}

void ApplicationControllerTest::
configuredNewWindowAndQuitPreserveOrder()
{
    QFETCH(QStringList, actions);
    QFETCH(int, expectedFactoryCalls);
    QFETCH(QStringList, expectedEvents);

    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+k=%1").arg(actions.constFirst()),
        QStringLiteral("chain=%1").arg(actions.constLast()),
    });
    ApplicationController controller(options, harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QTRY_COMPARE_WITH_TIMEOUT(initial->workspace->tabCount(), 1, 1000);
    TerminalPane *const pane = onlyPane(initial->workspace);
    QVERIFY(pane != nullptr);

    QStringList events;
    connect(&controller, &ApplicationController::windowCreated,
            &controller, [&events] {
                events.append(QStringLiteral("window"));
            });
    connect(&controller, &ApplicationController::applicationQuitCommitted,
            &controller, [&events] {
                events.append(QStringLiteral("quit"));
            });
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    sendCtrlKPressAndRelease(pane);

    QCOMPARE(harness.calls, 1);
    QCOMPARE(quit.count(), 0);
    QVERIFY(events.isEmpty());
    bool actionQueueDrained = false;
    QTimer::singleShot(0, &controller, [&actionQueueDrained] {
        actionQueueDrained = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(actionQueueDrained, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
    QCOMPARE(harness.calls, expectedFactoryCalls);
    QCOMPARE(events, expectedEvents);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
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

void ApplicationControllerTest::commandPrecedenceAndReloadUseFirstSessionLease()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    options.initialWindow = false;
    options.program.clear();
    options.hold = false;
    options.ordinaryCommand = TerminalCommand::direct({
        QByteArrayLiteral("/bin/sleep"),
        QByteArrayLiteral("30"),
    });
    options.initialCommand =
        TerminalCommand::shell(QByteArrayLiteral("exec sleep 30"));

    ApplicationController controller(options, harness.factory(), false);
    QVERIFY(controller.startWithoutInitialWindow());

    const TerminalCommand reloadedOrdinary = TerminalCommand::direct({
        QByteArrayLiteral("/bin/sleep"),
        QByteArrayLiteral("29"),
    });
    const TerminalCommand reloadedInitial =
        TerminalCommand::shell(QByteArrayLiteral("exec sleep 28"));
    LaunchOptions reloaded = options;
    reloaded.ordinaryCommand = reloadedOrdinary;
    reloaded.initialCommand = reloadedInitial;
    controller.applyLaunchOptions(reloaded);

    // No surface has reserved the one-shot lease, so reload replaces both the
    // initial command granted to the first starter and the ordinary fallback.
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);
    TerminalController *const first =
        onlyController(controller.windows().constFirst().workspace);
    QVERIFY(first != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(first->sessionStarted(), 2000);
    QVERIFY(first->launchProgram().isEmpty());
    QVERIFY(first->launchCommand()
            == std::optional<TerminalCommand>(reloadedInitial));
    QCOMPARE(first->launchTitle(), QStringLiteral("Terminal"));
    QVERIFY(!first->hasTitle());
    TerminalPane *const firstPane = controller.windows()
                                        .constFirst()
                                        .workspace->findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    QVERIFY(!firstPane->effectiveSurfaceTitle().has_value());

    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    TerminalController *const second =
        onlyController(controller.windows().constLast().workspace);
    QVERIFY(second != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(second->sessionStarted(), 2000);
    QVERIFY(second->launchProgram().isEmpty());
    QVERIFY(second->launchCommand()
            == std::optional<TerminalCommand>(reloadedOrdinary));
    QCOMPARE(second->launchTitle(), QStringLiteral("/bin/sleep"));
    QVERIFY(second->hasTitle());
    QCOMPARE(second->title(), QStringLiteral("/bin/sleep"));
    TerminalPane *const secondPane =
        controller.windows().constLast().workspace->findChild<TerminalPane *>();
    QVERIFY(secondPane != nullptr);
    QCOMPARE(secondPane->effectiveSurfaceTitle(),
             std::optional<QString>{QStringLiteral("/bin/sleep")});

    // The existing positional frontend command is already direct argv and has
    // higher first-session precedence than Ghostty initial-command. The
    // ordinary command remains the controller fallback but is not executed.
    WindowFactoryHarness cliHarness;
    LaunchOptions cli = reloaded;
    cli.initialWindow = true;
    cli.program = {QStringLiteral("/bin/true")};
    cli.hold = true;
    ApplicationController cliController(cli, cliHarness.factory(), false);
    const auto cliWindow = cliController.createInitialWindow();
    QVERIFY(cliWindow.has_value());
    TerminalController *const cliFirst = onlyController(cliWindow->workspace);
    QVERIFY(cliFirst != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(cliFirst->sessionStarted(), 2000);
    QCOMPARE(cliFirst->launchProgram(),
             QStringList({QStringLiteral("/bin/true")}));
    QVERIFY(cliFirst->launchCommand()
            == std::optional<TerminalCommand>(reloadedOrdinary));
    QCOMPARE(cliFirst->launchTitle(), QStringLiteral("true"));
    QVERIFY(cliFirst->hasTitle());
    QCOMPARE(cliFirst->title(), QStringLiteral("/bin/true"));
    TerminalPane *const cliPane =
        cliWindow->workspace->findChild<TerminalPane *>();
    QVERIFY(cliPane != nullptr);
    QCOMPARE(cliPane->effectiveSurfaceTitle(),
             std::optional<QString>{QStringLiteral("/bin/true")});
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
    reloaded.typography = sampleTypography(19.0);
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
    QVERIFY(first->effectiveLaunchOptions().typography
            == reloaded.typography);
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
    QCOMPARE(immediatePane->title(), QStringLiteral("/bin/sh"));

    QTRY_VERIFY_WITH_TIMEOUT(deferred->sessionStarted(), 2000);
    QVERIFY(deferred->launchProgram().isEmpty());
    QVERIFY(!deferred->launchHold());
    QCOMPARE(deferredPane->title(), QStringLiteral("Terminal"));
    const QStringList titles = created->workspace->tabTitles();
    QVERIFY(titles.contains(QStringLiteral("/bin/sh")));
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
    options.typography = sampleTypography(13.0);
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
    reloaded.typography = sampleTypography(11.0);
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
    QVERIFY(inherited.workspace->effectiveLaunchOptions().typography
            == typographyWithPointSize(reloaded.typography, 18.0));
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

void ApplicationControllerTest::approvedCloseUnmapsBeforeDestroyingWindow()
{
    WindowFactoryHarness harness;
    ApplicationController controller(baseOptions(QDir::currentPath()),
                                     harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());
    QTRY_VERIFY_WITH_TIMEOUT(initial->window->isVisible(), 1000);
    QVERIFY(initial->window->handle() != nullptr);

    QPointer<QQuickWindow> transient = new QQuickWindow;
    transient->setTransientParent(initial->window);
    transient->show();
    QTRY_VERIFY_WITH_TIMEOUT(transient->isVisible(), 1000);

    bool observedUnmap = false;
    bool nativeSurfaceSurvivedUnmap = false;
    connect(initial->window, &QWindow::visibleChanged, initial->window,
            [window = initial->window, &observedUnmap,
             &nativeSurfaceSurvivedUnmap](bool visible) {
                if (visible) return;
                observedUnmap = true;
                nativeSurfaceSurvivedUnmap = window->handle() != nullptr;
            });
    const QPointer<QQuickWindow> guardedWindow(initial->window);

    closeWorkspace(initial->workspace);

    QTRY_VERIFY_WITH_TIMEOUT(observedUnmap, 1000);
    QVERIFY(nativeSurfaceSurvivedUnmap);
    QVERIFY(transient != nullptr);
    QVERIFY(!transient->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(guardedWindow.isNull(), 1000);
    QCOMPARE(controller.windowCount(), 0);
    delete transient.data();
}

void ApplicationControllerTest::
    ordinaryCloseUsesOnlyTheFinalWindowForLifetimePolicy()
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
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
}

void ApplicationControllerTest::navigatesLiveWindowsInRegistrationOrder()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(options, harness.factory(), false);
    const auto firstCreated = controller.createInitialWindow();
    QVERIFY(firstCreated.has_value());
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 3, 1000);

    const QVector<ApplicationWindow> created = controller.windows();
    QCOMPARE(created.size(), 3);
    const ApplicationWindow first = created.at(0);
    const ApplicationWindow second = created.at(1);
    const ApplicationWindow third = created.at(2);
    TerminalPane *const firstPane = onlyPane(first.workspace);
    TerminalPane *const secondPane = onlyPane(second.workspace);
    TerminalPane *const thirdPane = onlyPane(third.workspace);
    QVERIFY(firstPane != nullptr);
    QVERIFY(secondPane != nullptr);
    QVERIFY(thirdPane != nullptr);

    first.window->requestActivate();
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), first.window, 1000);
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("goto_window:next")));
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), second.window, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        second.window->activeFocusItem(), secondPane, 1000);

    QVERIFY(controller.dispatch(WindowNavigationAction::Next));
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), third.window, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        third.window->activeFocusItem(), thirdPane, 1000);

    QVERIFY(controller.dispatch(WindowNavigationAction::Next));
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), first.window, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        first.window->activeFocusItem(), firstPane, 1000);

    QVERIFY(controller.dispatch(WindowNavigationAction::Previous));
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), third.window, 1000);

    // Presenting a minimized destination restores it without discarding a
    // simultaneous maximized state.
    first.window->requestActivate();
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), first.window, 1000);
    second.window->setWindowStates(
        Qt::WindowMinimized | Qt::WindowMaximized);
    QVERIFY(controller.dispatch(WindowNavigationAction::Next));
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), second.window, 1000);
    QVERIFY(!second.window->windowStates().testFlag(Qt::WindowMinimized));
    QVERIFY(second.window->windowStates().testFlag(Qt::WindowMaximized));
    second.window->setWindowStates(Qt::WindowNoState);

    // Reentrant navigation from destination focus must observe the newly
    // active root and safely begin another complete traversal.
    first.window->requestActivate();
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), first.window, 1000);
    bool nestedNavigation = false;
    const QMetaObject::Connection nestedConnection = connect(
        second.workspace, &TerminalWorkspace::workspaceActivated,
        &controller, [&] {
            if (nestedNavigation) return;
            nestedNavigation =
                controller.dispatch(WindowNavigationAction::Next);
        });
    QVERIFY(controller.dispatch(WindowNavigationAction::Next));
    QTRY_VERIFY_WITH_TIMEOUT(nestedNavigation, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), third.window, 1000);
    disconnect(nestedConnection);

    // Hidden roots remain registered but GTK does not present them.
    second.window->hide();
    first.window->requestActivate();
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), first.window, 1000);
    QVERIFY(controller.dispatch(WindowNavigationAction::Next));
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), third.window, 1000);
    second.window->show();
    first.window->requestActivate();
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), first.window, 1000);

    // A root whose workspace has committed close is also ineligible during
    // the queued interval before its QML window is retired.
    second.workspace->requestWindowClose();
    QVERIFY(!second.workspace->canHostApplicationQuitConfirmation());
    QVERIFY(controller.dispatch(WindowNavigationAction::Next));
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), third.window, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);

    // Destination focus can schedule its own retirement. The controller must
    // not retain an unguarded root/workspace pair after requestActivate() and
    // focusActivePane() return.
    first.window->requestActivate();
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), first.window, 1000);
    QPointer<QQuickWindow> reentrantTarget(third.window);
    connect(
        thirdPane, &TerminalPane::activated, &controller,
        [reentrantTarget] {
            if (reentrantTarget != nullptr) {
                reentrantTarget->deleteLater();
            }
        },
        Qt::SingleShotConnection);
    QVERIFY(controller.dispatch(WindowNavigationAction::Next));
    QTRY_VERIFY_WITH_TIMEOUT(reentrantTarget.isNull(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 1, 1000);

    first.window->requestActivate();
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), first.window, 1000);
    QVERIFY(!controller.dispatch(WindowNavigationAction::Next));
    QVERIFY(!controller.dispatch(WindowNavigationAction::Previous));

    // With no registered window active, GTK tests the first top-level before
    // advancing. A sole visible inactive Ghostty window is therefore a valid
    // destination rather than a no-peer failure.
    QQuickWindow external;
    external.show();
    external.requestActivate();
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), &external, 1000);
    QVERIFY(controller.dispatch(WindowNavigationAction::Previous));
    QTRY_COMPARE_WITH_TIMEOUT(
        QGuiApplication::focusWindow(), first.window, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        first.window->activeFocusItem(), firstPane, 1000);
    external.close();

    closeWorkspace(first.workspace);
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
    initialOptions.typography = sampleTypography(13.0);
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
    currentOptions.typography = sampleTypography(11.0);
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
    QVERIFY(inherited->effectiveLaunchOptions().typography
            == currentOptions.typography);
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
    QVERIFY(configured->effectiveLaunchOptions().typography
            == currentOptions.typography);
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

void ApplicationControllerTest::
controllerMayBeDestroyedDuringWindowCreation_data()
{
    QTest::addColumn<QString>("stage");
    QTest::newRow("factory") << QStringLiteral("factory");
    QTest::newRow("workspace-initialization")
        << QStringLiteral("initialization");
    QTest::newRow("window-created-observer") << QStringLiteral("created");
}

void ApplicationControllerTest::
controllerMayBeDestroyedDuringWindowCreation()
{
    QFETCH(QString, stage);
    ApplicationController *controller = nullptr;
    QPointer<ApplicationController> guardedController;
    QPointer<QQuickWindow> guardedWindow;
    QPointer<TerminalWorkspace> guardedWorkspace;
    const auto destroyController = [&] {
        delete std::exchange(controller, nullptr);
    };

    ApplicationController::WindowFactory factory = [&]
        -> std::expected<ApplicationWindow, QString> {
        guardedWindow = new QQuickWindow;
        guardedWorkspace = new TerminalWorkspace(
            guardedWindow->contentItem());
        guardedWorkspace->setParentItem(guardedWindow->contentItem());
        if (stage == QLatin1StringView("initialization")) {
            connect(guardedWorkspace,
                    &TerminalWorkspace::tabTitlesChanged,
                    this, destroyController,
                    Qt::SingleShotConnection);
        }
        const ApplicationWindow result{
            guardedWindow.data(), guardedWorkspace.data()};
        if (stage == QLatin1StringView("factory")) {
            destroyController();
        }
        return result;
    };

    controller = new ApplicationController(
        baseOptions(QDir::currentPath()), std::move(factory), false);
    guardedController = controller;
    if (stage == QLatin1StringView("created")) {
        connect(controller, &ApplicationController::windowCreated,
                this, destroyController,
                Qt::SingleShotConnection);
    }

    const auto created = controller->createInitialWindow();

    QVERIFY(guardedController.isNull());
    QVERIFY(!created.has_value());
    QVERIFY(guardedWindow.isNull());
    QVERIFY(guardedWorkspace.isNull());
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

void ApplicationControllerTest::explicitQuitWaitsForWindowRetirement()
{
    WindowFactoryHarness harness;
    ApplicationController controller(baseOptions(QDir::currentPath()),
                                     harness.factory(), false);
    const auto initial = controller.createInitialWindow();
    QVERIFY(initial.has_value());

    int windowsAtCommit = -1;
    connect(&controller, &ApplicationController::applicationQuitCommitted,
            &controller, [&controller, &windowsAtCommit] {
                windowsAtCommit = controller.windowCount();
            });
    QSignalSpy committed(&controller,
                         &ApplicationController::applicationQuitCommitted);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);

    QVERIFY(controller.dispatch(ApplicationAction::Quit));

    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    QCOMPARE(windowsAtCommit, 0);
    QCOMPARE(controller.windowCount(), 0);
    QCOMPARE(quit.count(), 1);
}

void ApplicationControllerTest::
    explicitQuitAggregatesEveryWindowIntoOneConfirmation()
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
    QTRY_COMPARE_WITH_TIMEOUT(firstClose.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(secondClose.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
    QVERIFY(!controller.activateNoCommand());
    QCOMPARE(creationFailure.count(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 0, 1000);
}

void ApplicationControllerTest::
reentrantQuitDuringClosePublicationDoesNotLoseApproval()
{
    WindowFactoryHarness harness;
    LaunchOptions options = baseOptions(QDir::currentPath());
    ApplicationController controller(options, harness.factory(), false);
    const auto first = controller.createInitialWindow();
    QVERIFY(first.has_value());
    QVERIFY(controller.dispatch(ApplicationAction::NewWindow));
    QTRY_COMPARE_WITH_TIMEOUT(controller.windowCount(), 2, 1000);
    const ApplicationWindow active = controller.windows().constLast();
    QCOMPARE(controller.activeWorkspace(), active.workspace);
    const QPointer<TerminalWorkspace> publishingWorkspace(first->workspace);

    bool reentrantQuitAccepted = false;
    bool publishingWorkspaceRetiredAtCommit = false;
    int windowsAtCommit = -1;
    connect(first->workspace, &TerminalWorkspace::windowCloseApproved,
            &controller, [&] {
                reentrantQuitAccepted =
                    controller.dispatch(ApplicationAction::Quit);
            });
    QSignalSpy committed(&controller,
                         &ApplicationController::applicationQuitCommitted);
    QSignalSpy quit(&controller, &ApplicationController::quitRequested);
    connect(&controller, &ApplicationController::applicationQuitCommitted,
            &controller, [&] {
                publishingWorkspaceRetiredAtCommit =
                    publishingWorkspace == nullptr;
                windowsAtCommit = controller.windowCount();
            });

    // The active host publishes first. While the other workspace is inside
    // its direct approval signal, request process-wide quit from a later
    // observer. A publication already in flight must not be inserted into the
    // controller's wait set after its one acknowledgement has passed.
    active.workspace->requestWindowClose();
    first->workspace->requestWindowClose();

    QTRY_VERIFY_WITH_TIMEOUT(reentrantQuitAccepted, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    QVERIFY(publishingWorkspaceRetiredAtCommit);
    QCOMPARE(windowsAtCommit, 0);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
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
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
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
