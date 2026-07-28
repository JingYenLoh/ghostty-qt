#include "ghostty_application_keybindings.h"
#include "launch_options.h"
#include "terminal_cell_metrics.h"
#include "terminal_controller.h"
#include "terminal_geometry.h"
#include "terminal_pane.h"
#include "terminal_pane_render_probe_p.h"
#include "terminal_typography.h"
#include "terminal_workspace.h"

#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUrl>
#include <QtQml/qqml.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

static_assert(std::is_same_v<
              decltype(std::declval<TerminalWorkspace &>().tabModel()),
              const TabListModel *>);

class ShellEnvironment final {
public:
    explicit ShellEnvironment(
        const QByteArray &shell = QByteArrayLiteral("/bin/sh"))
        : wasSet_(qEnvironmentVariableIsSet("SHELL"))
        , previous_(qgetenv("SHELL"))
    {
        qputenv("SHELL", shell);
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

class KeyReleaseReceiver final : public QObject {
public:
    int releaseCount() const noexcept
    {
        return releaseCount_;
    }

protected:
    bool event(QEvent *event) override
    {
        if (event != nullptr
            && event->type() == QEvent::KeyRelease) {
            ++releaseCount_;
        }
        return QObject::event(event);
    }

private:
    int releaseCount_ = 0;
};

class ResistantShellFixture final {
public:
    bool create(const QString &root)
    {
        readyDirectory_ = QDir(root).filePath(QStringLiteral("ready"));
        shellPath_ = QDir(root).filePath(QStringLiteral("resistant-shell"));
        if (!QDir().mkpath(readyDirectory_)) return false;

        QFile shellFile(shellPath_);
        if (!shellFile.open(QIODevice::WriteOnly)) return false;
        const QByteArray script = QByteArrayLiteral(
            "#!/bin/sh\n"
            "ready_dir=\"${0%/*}/ready\"\n"
            "trap '' HUP\n"
            ": > \"$ready_dir/$$\"\n"
            "exec /bin/sleep 30\n");
        if (shellFile.write(script) != script.size()) return false;
        shellFile.close();
        return shellFile.setPermissions(QFileDevice::ReadOwner
                                        | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner);
    }

    QByteArray encodedShellPath() const
    {
        return QFile::encodeName(shellPath_);
    }

    int readyProcessCount() const
    {
        return QDir(readyDirectory_)
            .entryList(QDir::Files | QDir::NoDotAndDotDot).size();
    }

private:
    QString shellPath_;
    QString readyDirectory_;
};

LaunchOptions baseOptions()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    return options;
}

TerminalTypography testTypography(QString prefix, double pointSize)
{
    TerminalTypography typography;
    typography.pointSize = pointSize;
    typography.face(TerminalFontRole::Regular) = {
        .families = {
            prefix + QStringLiteral(" Regular"),
            prefix + QStringLiteral(" Regular Fallback"),
        },
        .style = TerminalFontStyles::Named{
            prefix + QStringLiteral(" Book"),
        },
    };
    typography.face(TerminalFontRole::Bold) = {
        .families = {
            prefix + QStringLiteral(" Bold"),
            prefix + QStringLiteral(" Bold Fallback"),
        },
        .style = TerminalFontStyles::Named{
            prefix + QStringLiteral(" Heavy"),
        },
    };
    typography.face(TerminalFontRole::Italic) = {
        .families = {
            prefix + QStringLiteral(" Italic"),
        },
        .style = TerminalFontStyles::Disabled{},
    };
    typography.face(TerminalFontRole::BoldItalic) = {
        .families = {
            prefix + QStringLiteral(" Bold Italic"),
        },
        .style = TerminalFontStyles::Automatic{},
    };
    for (std::size_t index = 0;
         index < terminalEnumIndex(TerminalMetric::Count); ++index) {
        const TerminalMetric metric =
            static_cast<TerminalMetric>(index);
        if (index % 2 == 0) {
            typography.metricModifiers[metric] =
                TerminalMetricModifiers::Absolute{
                    static_cast<qint32>(index) - 4,
                };
        } else {
            typography.metricModifiers[metric] =
                TerminalMetricModifiers::Percentage{
                    1.0 + static_cast<double>(index) / 20.0,
                };
        }
    }
    return typography;
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

QList<QQuickItem *> splitDividerItems(TerminalWorkspace *workspace)
{
    return workspace->findChildren<QQuickItem *>(
        QStringLiteral("_ghosttyQtSplitDivider"),
        Qt::FindDirectChildrenOnly);
}

QPoint windowPoint(const TerminalWorkspace *workspace,
                   const QPointF &workspacePoint)
{
    return workspace->mapToScene(workspacePoint).toPoint();
}

bool approximatelyEqual(const QColor &left, const QColor &right)
{
    constexpr int tolerance = 2;
    return std::abs(left.red() - right.red()) <= tolerance
        && std::abs(left.green() - right.green()) <= tolerance
        && std::abs(left.blue() - right.blue()) <= tolerance
        && std::abs(left.alpha() - right.alpha()) <= tolerance;
}

TerminalActionResult successfulOpenFileResult(
    quint64 requestId, const QString &path)
{
    return {
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::OpenFile,
        .performed = true,
        .payload = path,
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    };
}

TerminalActionResult successfulTerminalActionResult(
    quint64 requestId,
    TerminalActionEffect effect = TerminalActionEffect::None,
    const QString &payload = {})
{
    return {
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = effect,
        .performed = true,
        .payload = payload,
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    };
}

QVector<TabId> tabIds(TerminalWorkspace &workspace)
{
    QVector<TabId> result;
    result.reserve(workspace.tabCount());
    for (int index = 0; index < workspace.tabCount(); ++index) {
        result.append(workspace.tabModel()->idAt(index));
    }
    return result;
}

quint64 closeConfirmationId(const QSignalSpy &requests, int index = -1)
{
    if (requests.isEmpty()) return 0;
    const int resolvedIndex = index >= 0 ? index : requests.size() - 1;
    return requests.at(resolvedIndex).constFirst().toULongLong();
}

struct CurrentTabProbe {
    TabId tabId;
    PaneId paneId;
    QPointer<TerminalPane> pane;
};

CurrentTabProbe currentTabProbe(TerminalWorkspace &workspace)
{
    const TabListEntry *const entry =
        workspace.tabModel()->entryAt(workspace.currentIndex());
    if (entry == nullptr) return {};

    TerminalPane *visiblePane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane->isVisible()) {
            if (visiblePane != nullptr) return {};
            visiblePane = pane;
        }
    }
    return {entry->id, entry->activePaneId, visiblePane};
}

CurrentTabProbe splitRightProbe(TerminalWorkspace &workspace)
{
    const QList<TerminalPane *> before =
        workspace.findChildren<TerminalPane *>();
    workspace.splitRight();

    const TabListEntry *const entry =
        workspace.tabModel()->entryAt(workspace.currentIndex());
    if (entry == nullptr) return {};
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (!before.contains(pane)) {
            return {entry->id, entry->activePaneId, pane};
        }
    }
    return {};
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
    return image.pixelColor(
        std::clamp(static_cast<int>(std::floor(scene.x() * xScale)),
                   0, image.width() - 1),
        std::clamp(static_cast<int>(std::floor(scene.y() * yScale)),
                   0, image.height() - 1));
}

bool dividerPaintsExactColor(QQuickWindow *window, QQuickItem *divider,
                             const QColor &expected,
                             bool requireDifferentNeighbors = true)
{
    const QImage image = window->grabWindow();
    if (image.isNull() || window->width() <= 0 || window->height() <= 0) {
        return false;
    }
    const QRectF sceneRect = divider->mapRectToScene(divider->boundingRect());
    const qreal xScale = static_cast<qreal>(image.width()) / window->width();
    const qreal yScale = static_cast<qreal>(image.height()) / window->height();
    const bool verticalDivider =
        divider->cursor().shape() == Qt::SplitHCursor;
    const qreal thicknessScale = verticalDivider ? xScale : yScale;
    const qreal start = verticalDivider ? sceneRect.left() : sceneRect.top();
    const qreal end = verticalDivider ? sceneRect.right() : sceneRect.bottom();
    const int physicalStart = qRound(start * thicknessScale);
    const int physicalEnd = qRound(end * thicknessScale);
    if (physicalEnd - physicalStart != qRound(2.0 * thicknessScale)
        || physicalStart <= 0) {
        return false;
    }

    const int alongLimit = verticalDivider ? image.height() : image.width();
    const int thicknessLimit = verticalDivider ? image.width() : image.height();
    const qreal alongScale = verticalDivider ? yScale : xScale;
    const qreal alongStart = verticalDivider
        ? sceneRect.top() : sceneRect.left();
    const qreal alongEnd = verticalDivider
        ? sceneRect.bottom() : sceneRect.right();
    const int physicalAlongStart = std::max(0, qRound(alongStart * alongScale));
    const int physicalAlongEnd = std::min(
        alongLimit, qRound(alongEnd * alongScale));
    if (physicalAlongStart >= physicalAlongEnd
        || physicalEnd >= thicknessLimit) {
        return false;
    }

    const auto pixel = [&](int thicknessPosition, int along) {
        return verticalDivider
            ? image.pixelColor(thicknessPosition, along)
            : image.pixelColor(along, thicknessPosition);
    };
    // Scan for a cross-section away from nested T-junctions. At that point,
    // every physical pixel in the half-open two-DIP stripe must be colored,
    // and both immediately adjacent terminal pixels must remain untouched.
    for (int along = physicalAlongStart; along < physicalAlongEnd; ++along) {
        bool filled = true;
        for (int position = physicalStart; position < physicalEnd; ++position) {
            if (!approximatelyEqual(pixel(position, along), expected)) {
                filled = false;
                break;
            }
        }
        if (filled
            && (!requireDifferentNeighbors
                || (!approximatelyEqual(
                        pixel(physicalStart - 1, along), expected)
                    && !approximatelyEqual(
                        pixel(physicalEnd, along), expected)))) {
            return true;
        }
    }
    return false;
}

} // namespace

class TerminalWorkspaceTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void initialGeometrySeedsOnlyFirstPane();
    void typographyReloadReachesLiveAndFuturePanes();
    void backgroundOpacityReloadIsPaneLocalAndInherited();
    void backgroundImageAndPaddingReloadStayPaneLocalAndInherited();
    void launchOnlyReloadAffectsOnlyFuturePanes();
    void inheritedTypographyChangesOnlyPointSize();
    void initializationSurvivesReentrantConfigurationObservers();
    void tabPublicationMayDestroyWorkspace();
    void deferredInitialSessionCancelsAndScopesToFirstPane();
    void keybindProgramStorageIsSharedWhilePaneStateIsLocal();
    void newerSameProgramWorkspaceUpdateWinsReentry();
    void runningProgramPromptsThenResolvesOnceOnExit();
    void distinguishesWindowCloseFromApplicationQuit();
    void configuredCloseChainSurvivesSynchronousWorkspaceDestruction();
    void configuredSurfaceCloseChainSurvivesSynchronousDestruction_data();
    void configuredSurfaceCloseChainSurvivesSynchronousDestruction();
    void broadCloseChainSurvivesSynchronousSourceDestruction_data();
    void broadCloseChainSurvivesSynchronousSourceDestruction();
    void allPaneClosePublicationOutlivesFanout();
    void broadFanoutSurvivesSynchronousWorkspaceDestruction();
    void unexpectedDirectObserverDestructionStopsChainSafely();
    void synchronousObserversMayDestroyWorkspace();
    void contextMenuRequestsUseStablePaneTargets();
    void contextMenuRoutesSurfaceAndSplitActions();
    void contextMenuRoutesTabActions();
    void contextMenuRoutesWindowAndConfigActions();
    void applicationQuitEscalatesCloseLifecycle();
    void closingOnlyPaneRemovesTab();
    void closeSurfaceUsesStableOriginsAndAdjacentFocus();
    void closeSurfaceConfirmationIsStableAndAtomic();
    void closeSurfaceConfirmsRunningProcess();
    void broadCloseSurfaceConvergesOnWorkspaceQuit();
    void idleShellDoesNotPromptInRunningProcessesMode();
    void submittedCommandPromptsBeforeForegroundPoll();
    void terminalControlSubmissionPromptsBeforeWorkerRoundTrip();
    void readOnlyBlocksUiActivityLatchAndProtectsClose();
    void readOnlyStateIsPaneLocalAndBroadFanoutIsStable();
    void abnormalExitOverlayPresentsHeldFailureAndCloses();
    void overlayComponentsShareOneLifecycle();
    void pendingPaneReloadsDuringOverlayCompletion();
    void resizeOverlayIsPaneLocalAndScalesWithDpr();
    void scrollbarOverlayIsPaneLocalAndReloadable();
    void bellBorderOverlayIsPaneLocalAndInputTransparent();
    void readOnlyNaturalExitPromptsExactlyOnce();
    void queuesAndCorrelatesUnsafePasteConfirmations();
    void ordersAndConfirmsTerminalClipboardWrites();
    void performableTabChangeRequiresDifferentTarget();
    void relativeTabActionsUseCurrentSelectionAndBroadFanout();
    void alwaysModePromptsForIdleShell();
    void multiPaneShutdownGracePeriodsOverlap();
    void closeTabModesUseStableOriginsAndPreserveFocus();
    void closeTabBatchConfirmationKeepsStableTargets();
    void closeTabResponsesUseStableConfirmationIds();
    void closeTabBatchRejectsReentrantTopologyChanges();
    void pendingCloseTargetsPruneBeforeModelPublication();
    void closeResponseDefersDuringBatchMutation();
    void naturalTabExitPrunesPendingBatchTarget();
    void broadCloseTabModesUseFirstStableSource();
    void closeTabBatchShutdownGracePeriodsOverlap();
    void rootApplicationBindingPrecedesActiveTable();
    void windowNavigationRetainsSurfaceScopeAndBroadFanout();
    void broadBindingsReachInactivePanesAndIgnoreLocalFlags();
    void broadPasteActionsReachEveryPane();
    void broadWriteScreenOpenCreatesDistinctPerPaneArtifacts();
    void broadTerminalBarrierOrdersCommitsAndDefersInputs();
    void broadSelectionBarriersOrderEffects();
    void broadRetainedSearchResultExpiresOnSessionExit();
    void searchEffectDestructionDefersBroadContinuation();
    void broadDeferredInputFifoSurvivesReplayReentrancy();
    void broadTerminalBarrierSkipsDestroyedTargets();
    void finalBroadTargetDestructionDefersKeyReplay();
    void destroyedDeferredReleaseClearsGlobalConsumption();
    void broadViewportAndSelectionActionsReachEveryPane();
    void indexedLastAndMovedTabsPreserveStableIds();
    void surfaceBaseTitlesFollowStablePanesAndOscUpdates();
    void tabTitleOverridesFollowStableSourcesAndReset();
    void bellFeedbackRoutesAcrossPaneTabWindowAndReload();
    void bellAttentionPublicationMayDestroyWorkspace();
    void surfaceTitlePromptsPreserveStableTargetsAndLayers();
    void broadSurfaceTitlePromptsShareFifoAndPruneRemovedPanes();
    void tabTitlePromptsPreserveStableTargetsAndReset();
    void broadTabTitlePromptsQueueEverySurfaceAndSurviveRemoval();
    void newTabPositionReloadsAndKeepsBroadOrder();
    void tabBarVisibilityTracksPolicyAndCount();
    void tabsLocationReloadsAsPresentationPolicy();
    void splitDirectionsPlaceAndFocusNewPane_data();
    void splitDirectionsPlaceAndFocusNewPane();
    void automaticSplitUsesOriginatingPaneAspect();
    void splitNavigationWrapsInTreeAndSpatialOrder();
    void relativeSplitNavigationUsesExplicitSourceAndTreeOrder();
    void splitResizeAndEqualizeRespectTreeAxes();
    void dragsExactNestedSplitDividerAndPreservesFocus();
    void splitDividerColorReloadsWithoutRelayout();
    void dimsUnfocusedSplitPanesAcrossLifecycle();
    void splitWorkingDirectoryPolicyReloadsForFutureNestedSplits();
    void newTabInheritanceUsesStableSourceAndReloadedPolicies();
    void splitDividerHitRegionPreservesTerminalInputAndZoom();
    void splitDividerDragClampsPersistsAndCancels();
    void splitZoomPreservesLayoutAndResetsOnNavigationAndSplit();
    void splitZoomNavigationPolicyReloadsLive();
    void broadContainerActionsResolveFromActivePane();
    void routesWindowStateActionsToHostWindows();
    void inactiveTabResizeAppliesWhenActivated();
};

void TerminalWorkspaceTest::initTestCase()
{
    qmlRegisterUncreatableType<TerminalPane>(
        "GhosttyQt", 1, 0, "TerminalPane",
        "TerminalPane instances are owned by TerminalWorkspace");
}

void TerminalWorkspaceTest::initialGeometrySeedsOnlyFirstPane()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(640.0, 360.0));
    QVERIFY(workspace.initialize(options));
    QCOMPARE(workspace.tabCount(), 1);

    const CurrentTabProbe first = currentTabProbe(workspace);
    QVERIFY(first.pane != nullptr);
    QVERIFY(terminalPaneRenderProbe(first.pane).initialGeometry.has_value());
    TerminalController *const firstController =
        first.pane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QVERIFY(firstController->sessionStarted());

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    const CurrentTabProbe second = currentTabProbe(workspace);
    QVERIFY(second.pane != nullptr);
    QVERIFY(second.pane != first.pane);
    QVERIFY(!terminalPaneRenderProbe(second.pane).initialGeometry.has_value());
    TerminalController *const secondController =
        second.pane->findChild<TerminalController *>();
    QVERIFY(secondController != nullptr);
    QVERIFY(secondController->sessionStarted());

    const CurrentTabProbe third = splitRightProbe(workspace);
    QVERIFY(third.pane != nullptr);
    QVERIFY(third.pane != second.pane);
    QVERIFY(!terminalPaneRenderProbe(third.pane).initialGeometry.has_value());
    TerminalController *const thirdController =
        third.pane->findChild<TerminalController *>();
    QVERIFY(thirdController != nullptr);
    QVERIFY(thirdController->sessionStarted());
}

void TerminalWorkspaceTest::typographyReloadReachesLiveAndFuturePanes()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.typography = testTypography(QStringLiteral("Initial"), 11.0);
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(640.0, 360.0));
    QVERIFY(workspace.initialize(options));
    workspace.splitRight();
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 2);

    LaunchOptions reloaded = options;
    reloaded.typography =
        testTypography(QStringLiteral("Reloaded"), 17.0);
    workspace.applyLaunchOptions(reloaded);
    QVERIFY(workspace.effectiveLaunchOptions().typography
            == reloaded.typography);

    const auto verifyTypography =
        [&reloaded](const TerminalPane *pane) {
            QVERIFY(pane != nullptr);
            const LaunchOptions inherited =
                pane->splitLaunchOptions(reloaded);
            QVERIFY(inherited.typography == reloaded.typography);
            QCOMPARE(pane->fontPointSize(),
                     reloaded.typography.pointSize);
        };
    for (const TerminalPane *pane :
         workspace.findChildren<TerminalPane *>()) {
        verifyTypography(pane);
    }

    workspace.newTab();
    const CurrentTabProbe tab = currentTabProbe(workspace);
    QVERIFY(tab.pane != nullptr);
    verifyTypography(tab.pane);

    const CurrentTabProbe split = splitRightProbe(workspace);
    QVERIFY(split.pane != nullptr);
    verifyTypography(split.pane);

    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 4);
    for (const TerminalPane *pane :
         workspace.findChildren<TerminalPane *>()) {
        verifyTypography(pane);
    }
}

void TerminalWorkspaceTest::backgroundOpacityReloadIsPaneLocalAndInherited()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 10"),
    };
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor =
        QColor(QStringLiteral("#101820"));
    options.background = {
        .opacity = 0.25,
        .opacityCells = false,
    };
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.setColor(Qt::transparent);
    window.resize(720, 360);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    QVERIFY(workspace->initialize(options));
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);

    const CurrentTabProbe first = currentTabProbe(*workspace);
    QVERIFY(first.pane != nullptr);
    const CurrentTabProbe second = splitRightProbe(*workspace);
    QVERIFY(second.pane != nullptr);
    QVERIFY(second.pane != first.pane);
    QCOMPARE(workspace->findChildren<TerminalPane *>().size(), 2);

    const QPointer<TerminalController> firstController =
        first.pane->findChild<TerminalController *>();
    const QPointer<TerminalController> secondController =
        second.pane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QVERIFY(secondController != nullptr);
    QVERIFY(firstController->sessionStarted());
    QVERIFY(secondController->sessionStarted());
    const QPointer<QThread> firstWorkerThread =
        firstController->findChild<QThread *>();
    const QPointer<QThread> secondWorkerThread =
        secondController->findChild<QThread *>();
    QVERIFY(firstWorkerThread != nullptr);
    QVERIFY(secondWorkerThread != nullptr);

    quint64 contentRevision = 1;
    const auto publishFrame =
        [&contentRevision](TerminalPane *pane, const QColor &background,
                           const QColor &explicitBackground) {
            TerminalController *const controller =
                pane->findChild<TerminalController *>();
            if (controller == nullptr) return false;

            TerminalUpdate update;
            update.columns = 2;
            update.rows = 1;
            update.fullFrame = true;
            update.colorsChanged = true;
            update.foreground = Qt::white;
            update.background = background;
            update.cursorColor = Qt::white;
            update.cursorChanged = true;
            update.cursorVisible = false;
            update.contentRevision = contentRevision++;

            TerminalRowUpdate row;
            row.row = 0;
            row.cells.resize(update.columns);
            for (TerminalCell &cell : row.cells) {
                cell.foreground = Qt::white;
                cell.background = background;
            }
            row.cells[1].background = explicitBackground;
            row.cells[1].backgroundExplicit = true;
            update.dirtyRows.append(std::move(row));

            controller->terminalUpdated(update);
            // The test-owned frame must remain authoritative while the real
            // workers continue running in the background.
            QObject::disconnect(
                controller, &TerminalController::terminalUpdated, pane,
                nullptr);
            pane->update();
            return true;
        };

    const QColor firstBackground(QStringLiteral("#203040"));
    const QColor firstExplicit(QStringLiteral("#904020"));
    const QColor secondBackground(QStringLiteral("#405060"));
    const QColor secondExplicit(QStringLiteral("#209060"));
    QVERIFY(publishFrame(first.pane, firstBackground, firstExplicit));
    QVERIFY(publishFrame(second.pane, secondBackground, secondExplicit));
    QVERIFY(!window.grabWindow().isNull());

    const auto withAlpha = [](QColor color, int alpha) {
        color.setAlpha(alpha);
        return color;
    };
    const auto verifyBackgroundLayers =
        [&withAlpha](const TerminalPaneRenderProbeSnapshot &probe,
                     const QColor &background,
                     const QColor &explicitBackground, int baseAlpha,
                     int explicitAlpha) {
            QCOMPARE(probe.baseBackground,
                     withAlpha(background, baseAlpha));
            QCOMPARE(probe.cellBackgrounds.size(), 2);
            QCOMPARE(probe.cellBackgrounds.at(0).alpha(), 0);
            QCOMPARE(probe.cellBackgrounds.at(1),
                     withAlpha(explicitBackground, explicitAlpha));
        };
    const TerminalPaneRenderProbeSnapshot firstInitial =
        terminalPaneRenderProbe(first.pane);
    const TerminalPaneRenderProbeSnapshot secondInitial =
        terminalPaneRenderProbe(second.pane);
    verifyBackgroundLayers(firstInitial, firstBackground, firstExplicit, 64,
                           255);
    verifyBackgroundLayers(secondInitial, secondBackground, secondExplicit, 64,
                           255);
    QVERIFY(firstInitial.rootSerial != 0);
    QVERIFY(secondInitial.rootSerial != 0);

    const QRectF firstGeometry(first.pane->position(), first.pane->size());
    const QRectF secondGeometry(second.pane->position(), second.pane->size());
    const PaneId activePaneId =
        workspace->tabModel()->entryAt(workspace->currentIndex())
            ->activePaneId;
    QSignalSpy firstResize(firstController,
                           &TerminalController::resizeRequested);
    QSignalSpy secondResize(secondController,
                            &TerminalController::resizeRequested);
    QSignalSpy firstRuntime(
        firstController, &TerminalController::runtimeOptionsRequested);
    QSignalSpy secondRuntime(
        secondController, &TerminalController::runtimeOptionsRequested);

    LaunchOptions reloaded = options;
    reloaded.background = {
        .opacity = 0.5,
        .opacityCells = true,
    };
    workspace->applyLaunchOptions(reloaded);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(workspace->effectiveLaunchOptions().background
            == reloaded.background);

    const TerminalPaneRenderProbeSnapshot firstReloaded =
        terminalPaneRenderProbe(first.pane);
    const TerminalPaneRenderProbeSnapshot secondReloaded =
        terminalPaneRenderProbe(second.pane);
    verifyBackgroundLayers(firstReloaded, firstBackground, firstExplicit, 128,
                           127);
    verifyBackgroundLayers(secondReloaded, secondBackground, secondExplicit,
                           128, 127);
    QCOMPARE(firstReloaded.rootSerial, firstInitial.rootSerial);
    QCOMPARE(secondReloaded.rootSerial, secondInitial.rootSerial);
    QVERIFY(firstReloaded.paintSerial > firstInitial.paintSerial);
    QVERIFY(secondReloaded.paintSerial > secondInitial.paintSerial);
    QVERIFY(first.pane != nullptr);
    QVERIFY(second.pane != nullptr);
    QVERIFY(firstController != nullptr);
    QVERIFY(secondController != nullptr);
    QVERIFY(firstWorkerThread != nullptr);
    QVERIFY(secondWorkerThread != nullptr);
    QCOMPARE(first.pane->findChild<TerminalController *>(),
             firstController.data());
    QCOMPARE(second.pane->findChild<TerminalController *>(),
             secondController.data());
    QCOMPARE(firstController->findChild<QThread *>(),
             firstWorkerThread.data());
    QCOMPARE(secondController->findChild<QThread *>(),
             secondWorkerThread.data());
    QVERIFY(firstController->sessionStarted());
    QVERIFY(secondController->sessionStarted());
    QCOMPARE(QRectF(first.pane->position(), first.pane->size()),
             firstGeometry);
    QCOMPARE(QRectF(second.pane->position(), second.pane->size()),
             secondGeometry);
    QCOMPARE(workspace->tabModel()->entryAt(workspace->currentIndex())
                 ->activePaneId,
             activePaneId);
    QCOMPARE(firstResize.count(), 0);
    QCOMPARE(secondResize.count(), 0);
    QCOMPARE(firstRuntime.count(), 0);
    QCOMPARE(secondRuntime.count(), 0);

    const CurrentTabProbe third = splitRightProbe(*workspace);
    QVERIFY(third.pane != nullptr);
    QVERIFY(third.pane != first.pane);
    QVERIFY(third.pane != second.pane);
    TerminalController *const thirdController =
        third.pane->findChild<TerminalController *>();
    QVERIFY(thirdController != nullptr);
    QVERIFY(thirdController->sessionStarted());

    const QColor thirdBackground(QStringLiteral("#607080"));
    const QColor thirdExplicit(QStringLiteral("#806020"));
    QVERIFY(publishFrame(third.pane, thirdBackground, thirdExplicit));
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot thirdPainted =
        terminalPaneRenderProbe(third.pane);
    verifyBackgroundLayers(thirdPainted, thirdBackground, thirdExplicit, 128,
                           127);
    QVERIFY(thirdPainted.rootSerial != 0);

    workspace.reset();
    window.close();
}

void TerminalWorkspaceTest::
    backgroundImageAndPaddingReloadStayPaneLocalAndInherited()
{
    const QString temporaryRoot =
        QDir::current().filePath(QStringLiteral("tmp"));
    QVERIFY(QDir().mkpath(temporaryRoot));
    QTemporaryDir temporary(
        QDir(temporaryRoot)
            .filePath(QStringLiteral("terminal-workspace-backdrop-XXXXXX")));
    QVERIFY(temporary.isValid());

    constexpr QSize sourceSize(32, 16);
    const QString sourcePath =
        temporary.filePath(QStringLiteral("split-backdrop.png"));
    QImage source(sourceSize, QImage::Format_RGBA8888);
    source.fill(QColor(QStringLiteral("#20a0d0")));
    QVERIFY(source.save(sourcePath, "PNG"));

    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 10"),
    };
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = QColor(QStringLiteral("#102030"));
    options.background.image = {
        .path =
            GhosttyConfigPath{
                .path = sourcePath,
                .optional = false,
            },
        .opacity = 0.8,
        .position = TerminalBackgroundImagePosition::BottomRight,
        .fit = TerminalBackgroundImageFit::None,
        .repeat = false,
    };
    options.padding = {
        .horizontal = {.leadingPoints = 9, .trailingPoints = 15},
        .vertical = {.leadingPoints = 6, .trailingPoints = 12},
        .balance = TerminalPaddingBalance::Disabled,
        .color = TerminalPaddingColor::Background,
    };
    options.splitAppearance.unfocusedOpacity = 1.0;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.setColor(Qt::transparent);
    window.resize(721, 360);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    QVERIFY(workspace->initialize(options));
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);

    const CurrentTabProbe first = currentTabProbe(*workspace);
    QVERIFY(first.pane != nullptr);
    const CurrentTabProbe second = splitRightProbe(*workspace);
    QVERIFY(second.pane != nullptr);
    QVERIFY(second.pane != first.pane);
    QCOMPARE(workspace->findChildren<TerminalPane *>().size(), 2);

    const QPointer<TerminalController> firstController =
        first.pane->findChild<TerminalController *>();
    const QPointer<TerminalController> secondController =
        second.pane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QVERIFY(secondController != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(firstController->sessionStarted(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(secondController->sessionStarted(), 3000);
    const QPointer<QThread> firstWorkerThread =
        firstController->findChild<QThread *>();
    const QPointer<QThread> secondWorkerThread =
        secondController->findChild<QThread *>();
    QVERIFY(firstWorkerThread != nullptr);
    QVERIFY(secondWorkerThread != nullptr);

    const auto paintAll = [&] {
        for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
            pane->update();
        }
        return !window.grabWindow().isNull();
    };
    const auto initialImagesReady = [&] {
        if (!paintAll()) return false;
        return terminalPaneRenderProbe(first.pane).backgroundImageAssetSerial
            != 0
            && terminalPaneRenderProbe(second.pane).backgroundImageAssetSerial
            != 0;
    };
    QTRY_VERIFY_WITH_TIMEOUT(initialImagesReady(), 3000);

    const auto visiblePlacement =
        [&](const TerminalPane *pane,
            const TerminalBackgroundImageOptions &image) {
            return terminalBackgroundImagePlacement(
                       pane->boundingRect(), sourceSize,
                       window.devicePixelRatio(), image.fit, image.position)
                .intersected(pane->boundingRect());
        };
    const auto verifyPlacement =
        [&](const TerminalPane *pane,
            const TerminalPaneRenderProbeSnapshot &probe,
            const TerminalBackgroundImageOptions &image) {
            QCOMPARE(probe.backgroundImageRect, visiblePlacement(pane, image));
            QCOMPARE(probe.backgroundImageSourceRect,
                     QRectF(QPointF{}, QSizeF(sourceSize)));
            QVERIFY(pane->boundingRect().contains(probe.backgroundImageRect));
        };

    const TerminalPaneRenderProbeSnapshot firstInitial =
        terminalPaneRenderProbe(first.pane);
    const TerminalPaneRenderProbeSnapshot secondInitial =
        terminalPaneRenderProbe(second.pane);
    verifyPlacement(first.pane, firstInitial, options.background.image);
    verifyPlacement(second.pane, secondInitial, options.background.image);
    QCOMPARE(firstInitial.backgroundImageAssetSerial,
             secondInitial.backgroundImageAssetSerial);
    const quint64 assetSerial = firstInitial.backgroundImageAssetSerial;
    QVERIFY(assetSerial != 0);
    QVERIFY(firstInitial.rootSerial != 0);
    QVERIFY(secondInitial.rootSerial != 0);

    const QRectF firstGeometry(first.pane->position(), first.pane->size());
    const QRectF secondGeometry(second.pane->position(), second.pane->size());
    const PaneId activePaneId =
        workspace->tabModel()->entryAt(workspace->currentIndex())->activePaneId;
    QSignalSpy firstResize(firstController,
                           &TerminalController::resizeRequested);
    QSignalSpy secondResize(secondController,
                            &TerminalController::resizeRequested);
    QSignalSpy firstRuntime(firstController,
                            &TerminalController::runtimeOptionsRequested);
    QSignalSpy secondRuntime(secondController,
                             &TerminalController::runtimeOptionsRequested);

    LaunchOptions imageReload = options;
    imageReload.background.image.opacity = 0.45;
    imageReload.background.image.position =
        TerminalBackgroundImagePosition::TopLeft;
    workspace->applyLaunchOptions(imageReload);
    QVERIFY(paintAll());
    const TerminalPaneRenderProbeSnapshot firstImageReload =
        terminalPaneRenderProbe(first.pane);
    const TerminalPaneRenderProbeSnapshot secondImageReload =
        terminalPaneRenderProbe(second.pane);
    verifyPlacement(first.pane, firstImageReload, imageReload.background.image);
    verifyPlacement(second.pane, secondImageReload,
                    imageReload.background.image);
    QCOMPARE(firstImageReload.backgroundImageAssetSerial, assetSerial);
    QCOMPARE(secondImageReload.backgroundImageAssetSerial, assetSerial);
    QCOMPARE(firstImageReload.rootSerial, firstInitial.rootSerial);
    QCOMPARE(secondImageReload.rootSerial, secondInitial.rootSerial);
    QVERIFY(firstImageReload.paintSerial > firstInitial.paintSerial);
    QVERIFY(secondImageReload.paintSerial > secondInitial.paintSerial);
    QCOMPARE(QRectF(first.pane->position(), first.pane->size()), firstGeometry);
    QCOMPARE(QRectF(second.pane->position(), second.pane->size()),
             secondGeometry);
    QCOMPARE(
        workspace->tabModel()->entryAt(workspace->currentIndex())->activePaneId,
        activePaneId);
    QCOMPARE(first.pane->findChild<TerminalController *>(),
             firstController.data());
    QCOMPARE(second.pane->findChild<TerminalController *>(),
             secondController.data());
    QCOMPARE(firstController->findChild<QThread *>(), firstWorkerThread.data());
    QCOMPARE(secondController->findChild<QThread *>(),
             secondWorkerThread.data());
    QCOMPARE(firstResize.count(), 0);
    QCOMPARE(secondResize.count(), 0);
    QCOMPARE(firstRuntime.count(), 0);
    QCOMPARE(secondRuntime.count(), 0);

    const auto layoutFor = [&](const TerminalPane *pane,
                               const TerminalPaneRenderProbeSnapshot &probe,
                               const TerminalPaddingOptions &padding) {
        return terminalViewportLayout({
            .surfaceSize = pane->size(),
            .cellSize =
                QSizeF(probe.metrics.cellWidth, probe.metrics.cellHeight),
            .devicePixelRatio = window.devicePixelRatio(),
            .padding = padding,
        });
    };
    const auto firstInitialLayout =
        layoutFor(first.pane, firstImageReload, options.padding);
    const auto secondInitialLayout =
        layoutFor(second.pane, secondImageReload, options.padding);
    QVERIFY(firstInitialLayout.has_value());
    QVERIFY(secondInitialLayout.has_value());

    const QColor edgeBackground(QStringLiteral("#d050a0"));
    quint64 contentRevision = 1;
    const auto publishEdgeFrame = [&contentRevision, &edgeBackground](
                                      TerminalPane *pane,
                                      const TerminalViewportLayout &layout) {
        TerminalController *const controller =
            pane->findChild<TerminalController *>();
        if (controller == nullptr) return false;

        TerminalUpdate update;
        update.columns = layout.session.columns;
        update.rows = layout.session.rows;
        update.fullFrame = true;
        update.colorsChanged = true;
        update.foreground = Qt::white;
        update.background = QColor(QStringLiteral("#102030"));
        update.cursorColor = Qt::white;
        update.cursorChanged = true;
        update.cursorVisible = false;
        update.contentRevision = contentRevision++;
        update.dirtyRows.reserve(update.rows);
        for (int row = 0; row < update.rows; ++row) {
            TerminalRowUpdate rowUpdate;
            rowUpdate.row = row;
            rowUpdate.cells.resize(update.columns);
            for (TerminalCell &cell : rowUpdate.cells) {
                cell.foreground = Qt::white;
                cell.background = edgeBackground;
                cell.backgroundExplicit = true;
            }
            update.dirtyRows.append(std::move(rowUpdate));
        }
        controller->terminalUpdated(update);
        QObject::disconnect(controller, &TerminalController::terminalUpdated,
                            pane, nullptr);
        pane->update();
        return true;
    };
    QVERIFY(publishEdgeFrame(first.pane, *firstInitialLayout));
    QVERIFY(publishEdgeFrame(second.pane, *secondInitialLayout));
    QVERIFY(paintAll());
    const TerminalPaneRenderProbeSnapshot firstBeforePaddingPolicy =
        terminalPaneRenderProbe(first.pane);
    const TerminalPaneRenderProbeSnapshot secondBeforePaddingPolicy =
        terminalPaneRenderProbe(second.pane);

    LaunchOptions paddingReload = imageReload;
    paddingReload.padding = {
        .horizontal = {.leadingPoints = 30, .trailingPoints = 45},
        .vertical = {.leadingPoints = 24, .trailingPoints = 36},
        .balance = TerminalPaddingBalance::Equal,
        .color = TerminalPaddingColor::Background,
    };
    workspace->applyLaunchOptions(paddingReload);
    QTRY_VERIFY_WITH_TIMEOUT(!firstResize.isEmpty() && !secondResize.isEmpty(),
                             1000);
    QVERIFY(paintAll());

    TerminalPaddingOptions retainedPadding = options.padding;
    retainedPadding.balance = paddingReload.padding.balance;
    retainedPadding.color = paddingReload.padding.color;
    const TerminalPaneRenderProbeSnapshot firstPaddingReload =
        terminalPaneRenderProbe(first.pane);
    const TerminalPaneRenderProbeSnapshot secondPaddingReload =
        terminalPaneRenderProbe(second.pane);
    const auto firstRetainedLayout =
        layoutFor(first.pane, firstPaddingReload, retainedPadding);
    const auto secondRetainedLayout =
        layoutFor(second.pane, secondPaddingReload, retainedPadding);
    const auto firstFutureLayout =
        layoutFor(first.pane, firstPaddingReload, paddingReload.padding);
    const auto secondFutureLayout =
        layoutFor(second.pane, secondPaddingReload, paddingReload.padding);
    QVERIFY(firstRetainedLayout.has_value());
    QVERIFY(secondRetainedLayout.has_value());
    QVERIFY(firstFutureLayout.has_value());
    QVERIFY(secondFutureLayout.has_value());
    QCOMPARE(
        firstResize.constLast().constFirst().value<TerminalSessionGeometry>(),
        firstRetainedLayout->session);
    QCOMPARE(
        secondResize.constLast().constFirst().value<TerminalSessionGeometry>(),
        secondRetainedLayout->session);
    QVERIFY(firstRetainedLayout->session != firstFutureLayout->session);
    QVERIFY(secondRetainedLayout->session != secondFutureLayout->session);
    QCOMPARE(firstPaddingReload.backgroundImageAssetSerial, assetSerial);
    QCOMPARE(secondPaddingReload.backgroundImageAssetSerial, assetSerial);
    QCOMPARE(firstPaddingReload.rootSerial, firstInitial.rootSerial);
    QCOMPARE(secondPaddingReload.rootSerial, secondInitial.rootSerial);
    QVERIFY(firstPaddingReload.paintSerial
            > firstBeforePaddingPolicy.paintSerial);
    QVERIFY(secondPaddingReload.paintSerial
            > secondBeforePaddingPolicy.paintSerial);
    verifyPlacement(first.pane, firstPaddingReload,
                    paddingReload.background.image);
    verifyPlacement(second.pane, secondPaddingReload,
                    paddingReload.background.image);
    QCOMPARE(firstRuntime.count(), 0);
    QCOMPARE(secondRuntime.count(), 0);
    QCOMPARE(firstController->findChild<QThread *>(), firstWorkerThread.data());
    QCOMPARE(secondController->findChild<QThread *>(),
             secondWorkerThread.data());
    QCOMPARE(QRectF(first.pane->position(), first.pane->size()), firstGeometry);
    QCOMPARE(QRectF(second.pane->position(), second.pane->size()),
             secondGeometry);

    // Color is a live paint-only policy. Isolate it from the balancing
    // transition so a repaint cannot be mistaken for a grid resize or a
    // worker runtime update.
    const int firstBalanceResizeCount = firstResize.count();
    const int secondBalanceResizeCount = secondResize.count();
    LaunchOptions colorReload = paddingReload;
    colorReload.padding.color = TerminalPaddingColor::ExtendAlways;
    workspace->applyLaunchOptions(colorReload);
    QVERIFY(paintAll());
    const TerminalPaneRenderProbeSnapshot firstColorReload =
        terminalPaneRenderProbe(first.pane);
    const TerminalPaneRenderProbeSnapshot secondColorReload =
        terminalPaneRenderProbe(second.pane);
    QCOMPARE(firstResize.count(), firstBalanceResizeCount);
    QCOMPARE(secondResize.count(), secondBalanceResizeCount);
    QCOMPARE(firstRuntime.count(), 0);
    QCOMPARE(secondRuntime.count(), 0);
    QCOMPARE(firstColorReload.backgroundImageAssetSerial, assetSerial);
    QCOMPARE(secondColorReload.backgroundImageAssetSerial, assetSerial);
    QCOMPARE(firstColorReload.rootSerial, firstInitial.rootSerial);
    QCOMPARE(secondColorReload.rootSerial, secondInitial.rootSerial);
    QVERIFY(firstColorReload.paintSerial > firstPaddingReload.paintSerial);
    QVERIFY(secondColorReload.paintSerial > secondPaddingReload.paintSerial);
    QVERIFY(std::ranges::all_of(firstColorReload.cellBackgrounds,
                                [&edgeBackground](const QColor &color) {
                                    return color == edgeBackground;
                                }));
    QVERIFY(std::ranges::all_of(secondColorReload.cellBackgrounds,
                                [&edgeBackground](const QColor &color) {
                                    return color == edgeBackground;
                                }));
    verifyPlacement(first.pane, firstColorReload, colorReload.background.image);
    verifyPlacement(second.pane, secondColorReload,
                    colorReload.background.image);
    QCOMPARE(firstController->findChild<QThread *>(), firstWorkerThread.data());
    QCOMPARE(secondController->findChild<QThread *>(),
             secondWorkerThread.data());
    QCOMPARE(QRectF(first.pane->position(), first.pane->size()), firstGeometry);
    QCOMPARE(QRectF(second.pane->position(), second.pane->size()),
             secondGeometry);

    const CurrentTabProbe third = splitRightProbe(*workspace);
    QVERIFY(third.pane != nullptr);
    QVERIFY(third.pane != first.pane);
    QVERIFY(third.pane != second.pane);
    TerminalController *const thirdController =
        third.pane->findChild<TerminalController *>();
    QVERIFY(thirdController != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(thirdController->sessionStarted(), 3000);
    QThread *const thirdWorkerThread = thirdController->findChild<QThread *>();
    QVERIFY(thirdWorkerThread != nullptr);

    const auto inheritedImageReady = [&] {
        if (!paintAll()) return false;
        return terminalPaneRenderProbe(third.pane).backgroundImageAssetSerial
            != 0;
    };
    QTRY_VERIFY_WITH_TIMEOUT(inheritedImageReady(), 3000);
    const TerminalPaneRenderProbeSnapshot thirdInitial =
        terminalPaneRenderProbe(third.pane);
    QCOMPARE(thirdInitial.backgroundImageAssetSerial, assetSerial);
    QVERIFY(thirdInitial.rootSerial != 0);
    verifyPlacement(third.pane, thirdInitial, colorReload.background.image);
    QVERIFY(workspace->effectiveLaunchOptions().background
            == colorReload.background);
    QVERIFY(workspace->effectiveLaunchOptions().padding == colorReload.padding);

    QSignalSpy thirdResize(thirdController,
                           &TerminalController::resizeRequested);
    const QSizeF inheritedProbeSize(
        std::max<qreal>(1.0, third.pane->width() - 3.0),
        std::max<qreal>(1.0, third.pane->height() - 2.0));
    third.pane->setSize(inheritedProbeSize);
    QTRY_VERIFY_WITH_TIMEOUT(!thirdResize.isEmpty(), 1000);
    QVERIFY(paintAll());
    const TerminalPaneRenderProbeSnapshot thirdResized =
        terminalPaneRenderProbe(third.pane);
    const auto inheritedLayout =
        layoutFor(third.pane, thirdResized, colorReload.padding);
    const auto staleLayout =
        layoutFor(third.pane, thirdResized, retainedPadding);
    QVERIFY(inheritedLayout.has_value());
    QVERIFY(staleLayout.has_value());
    QCOMPARE(
        thirdResize.constLast().constFirst().value<TerminalSessionGeometry>(),
        inheritedLayout->session);
    QVERIFY(inheritedLayout->session != staleLayout->session);
    QCOMPARE(thirdResized.backgroundImageAssetSerial, assetSerial);
    QCOMPARE(thirdResized.rootSerial, thirdInitial.rootSerial);
    QCOMPARE(thirdController->findChild<QThread *>(), thirdWorkerThread);
    verifyPlacement(third.pane, thirdResized, colorReload.background.image);

    QVERIFY(publishEdgeFrame(third.pane, *inheritedLayout));
    QVERIFY(paintAll());
    const TerminalPaneRenderProbeSnapshot thirdWithInheritedColor =
        terminalPaneRenderProbe(third.pane);
    QCOMPARE(thirdWithInheritedColor.rootSerial, thirdInitial.rootSerial);
    QVERIFY(std::ranges::all_of(thirdWithInheritedColor.cellBackgrounds,
                                [&edgeBackground](const QColor &color) {
                                    return color == edgeBackground;
                                }));

    workspace.reset();
    window.close();
}

void TerminalWorkspaceTest::launchOnlyReloadAffectsOnlyFuturePanes()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.term = QByteArrayLiteral("ghostty-qt-initial");
    options.environment = {
        {
            .key = QByteArrayLiteral("GHOSTTY_QT_RELOAD"),
            .value = QByteArrayLiteral("initial"),
        },
        {
            .key = QByteArrayLiteral("INITIAL_ONLY"),
            .value = QByteArray::fromHex("ff01"),
        },
    };
    options.linuxCgroup = {
        .mode = LinuxCgroupMode::Never,
        .memoryLimitBytes = quint64{4096},
        .processesLimit = quint64{4},
        .hardFail = false,
    };
    options.processUsesSingleInstance = false;
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(640.0, 360.0));
    QVERIFY(workspace.initialize(options));
    const CurrentTabProbe first = currentTabProbe(workspace);
    QVERIFY(first.pane != nullptr);
    TerminalController *const firstController =
        first.pane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QCOMPARE(firstController->launchTerm(),
             QByteArrayLiteral("ghostty-qt-initial"));
    QCOMPARE(firstController->launchEnvironment(), options.environment);
    QCOMPARE(firstController->launchLinuxCgroup(), options.linuxCgroup);
    QVERIFY(!firstController->launchProcessUsesSingleInstance());
    QSignalSpy runtimeOptions(firstController,
                              &TerminalController::runtimeOptionsRequested);

    LaunchOptions reloaded = options;
    reloaded.term = QByteArrayLiteral("ghostty-qt-reloaded");
    reloaded.environment = {
        {
            .key = QByteArrayLiteral("GHOSTTY_QT_RELOAD"),
            .value = QByteArrayLiteral("reloaded"),
        },
        {
            .key = QByteArrayLiteral("RELOADED_ONLY"),
            .value = QByteArray::fromHex("fe02"),
        },
    };
    reloaded.linuxCgroup = {
        .mode = LinuxCgroupMode::SingleInstance,
        .memoryLimitBytes = std::numeric_limits<quint64>::max(),
        .processesLimit = quint64{0},
        .hardFail = true,
    };
    workspace.applyLaunchOptions(reloaded);
    QCOMPARE(workspace.effectiveLaunchOptions().term, reloaded.term);
    QCOMPARE(workspace.effectiveLaunchOptions().environment,
             reloaded.environment);
    QCOMPARE(workspace.effectiveLaunchOptions().linuxCgroup,
             reloaded.linuxCgroup);
    QCOMPARE(first.pane->findChild<TerminalController *>(), firstController);
    QCOMPARE(firstController->launchTerm(),
             QByteArrayLiteral("ghostty-qt-initial"));
    QCOMPARE(firstController->launchEnvironment(), options.environment);
    QCOMPARE(firstController->launchLinuxCgroup(), options.linuxCgroup);
    QCOMPARE(runtimeOptions.count(), 0);

    workspace.newTab();
    const CurrentTabProbe tab = currentTabProbe(workspace);
    QVERIFY(tab.pane != nullptr);
    QVERIFY(tab.pane != first.pane);
    TerminalController *const tabController =
        tab.pane->findChild<TerminalController *>();
    QVERIFY(tabController != nullptr);
    QCOMPARE(tabController->launchTerm(),
             QByteArrayLiteral("ghostty-qt-reloaded"));
    QCOMPARE(tabController->launchEnvironment(), reloaded.environment);
    QCOMPARE(tabController->launchLinuxCgroup(), reloaded.linuxCgroup);
    QVERIFY(!tabController->launchProcessUsesSingleInstance());

    const CurrentTabProbe split = splitRightProbe(workspace);
    QVERIFY(split.pane != nullptr);
    QVERIFY(split.pane != tab.pane);
    TerminalController *const splitController =
        split.pane->findChild<TerminalController *>();
    QVERIFY(splitController != nullptr);
    QCOMPARE(splitController->launchTerm(),
             QByteArrayLiteral("ghostty-qt-reloaded"));
    QCOMPARE(splitController->launchEnvironment(), reloaded.environment);
    QCOMPARE(splitController->launchLinuxCgroup(), reloaded.linuxCgroup);
    QVERIFY(!splitController->launchProcessUsesSingleInstance());
    QCOMPARE(firstController->launchTerm(),
             QByteArrayLiteral("ghostty-qt-initial"));
    QCOMPARE(firstController->launchEnvironment(), options.environment);
    QCOMPARE(firstController->launchLinuxCgroup(), options.linuxCgroup);
}

void TerminalWorkspaceTest::inheritedTypographyChangesOnlyPointSize()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions sourceOptions = baseOptions();
    sourceOptions.program = {QStringLiteral("/bin/true")};
    sourceOptions.hold = true;
    sourceOptions.confirmCloseMode = ConfirmCloseMode::Never;
    sourceOptions.typography =
        testTypography(QStringLiteral("Source"), 12.0);
    TerminalWorkspace::setDefaultLaunchOptions(sourceOptions);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(640.0, 360.0));
    QVERIFY(workspace.initialize(sourceOptions));
    const CurrentTabProbe source = currentTabProbe(workspace);
    QVERIFY(source.pane != nullptr);
    source.pane->zoomIn();
    QCOMPARE(source.pane->fontPointSize(), 13.0);

    LaunchOptions applicationOptions = sourceOptions;
    applicationOptions.typography =
        testTypography(QStringLiteral("Application"), 21.0);
    TerminalTypography expected = applicationOptions.typography;
    expected.pointSize = source.pane->fontPointSize();

    const LaunchOptions tabOptions =
        source.pane->tabLaunchOptions(applicationOptions);
    QVERIFY(tabOptions.typography == expected);

    const std::optional<LaunchOptions> windowOptions =
        workspace.newWindowLaunchOptions(
            applicationOptions, source.paneId);
    QVERIFY(windowOptions.has_value());
    QVERIFY(windowOptions->typography == expected);

    const LaunchOptions splitOptions =
        source.pane->splitLaunchOptions(applicationOptions);
    TerminalTypography expectedSplit = sourceOptions.typography;
    expectedSplit.pointSize = source.pane->fontPointSize();
    QVERIFY(splitOptions.typography == expectedSplit);
}

void TerminalWorkspaceTest::
initializationSurvivesReentrantConfigurationObservers()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions initial = baseOptions();
    initial.program = {QStringLiteral("/bin/true")};
    initial.hold = true;
    initial.windowShowTabBar = WindowShowTabBar::Auto;
    TerminalWorkspace::setDefaultLaunchOptions(initial);

    const GhosttyKeybindProgram initialProgram =
        GhosttyKeybindProgram::compile(initial.keybindSource).program;
    LaunchOptions requested = initial;
    requested.typography.pointSize = 10.0;
    requested.windowShowTabBar = WindowShowTabBar::Always;

    {
        auto *workspace = new TerminalWorkspace;
        const QPointer<TerminalWorkspace> guard(workspace);
        connect(workspace, &TerminalWorkspace::tabBarVisibleChanged,
                this, [workspace] { delete workspace; });

        QVERIFY(workspace->initialize(
            requested, TerminalSessionStartMode::Deferred, {},
            initialProgram));
        QVERIFY(guard.isNull());
    }

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(640.0, 360.0));
    LaunchOptions newer = initial;
    newer.typography.pointSize = 20.0;
    newer.windowShowTabBar = WindowShowTabBar::Never;
    newer.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+j=ignore"),
    });
    const GhosttyKeybindProgram newerProgram =
        GhosttyKeybindProgram::compile(newer.keybindSource).program;

    bool nested = false;
    connect(&workspace, &TerminalWorkspace::tabBarVisibleChanged,
            &workspace, [&] {
                if (nested) return;
                nested = true;
                workspace.applyLaunchOptions(newer, newerProgram);
            });

    QVERIFY(workspace.initialize(
        requested, TerminalSessionStartMode::Immediate, {}, initialProgram));
    QVERIFY(nested);
    QCOMPARE(workspace.effectiveLaunchOptions(), newer);
    QVERIFY(workspace.keybindProgram().isSameGeneration(newerProgram));

    const CurrentTabProbe pane = currentTabProbe(workspace);
    QVERIFY(pane.pane != nullptr);
    QVERIFY(pane.pane->keybindProgram().isSameGeneration(newerProgram));
    QCOMPARE(pane.pane->fontPointSize(), newer.typography.pointSize);
    TerminalController *const controller =
        pane.pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QVERIFY(controller->launchGeometry().has_value());
    const TerminalCellMetrics metrics = terminalCellMetrics(newer.typography);
    QCOMPARE(
        *controller->launchGeometry(),
        terminalSessionGeometryForViewport(
            workspace.width(), workspace.height(),
            metrics.cellWidth, metrics.cellHeight, 1.0));
}

void TerminalWorkspaceTest::tabPublicationMayDestroyWorkspace()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.windowShowTabBar = WindowShowTabBar::Auto;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    const auto initializedWorkspace = [&options] {
        auto *workspace = new TerminalWorkspace;
        workspace->setSize(QSizeF(640.0, 360.0));
        if (!workspace->initialize(options)) {
            delete workspace;
            return static_cast<TerminalWorkspace *>(nullptr);
        }
        return workspace;
    };

    {
        auto *workspace = new TerminalWorkspace;
        workspace->setSize(QSizeF(640.0, 360.0));
        const QPointer<TerminalWorkspace> guard(workspace);
        connect(workspace->tabModel(), &QAbstractItemModel::rowsInserted,
                this, [workspace] { delete workspace; });
        QVERIFY(workspace->initialize(
            options, TerminalSessionStartMode::Deferred));
        QVERIFY(guard.isNull());
    }

    {
        TerminalWorkspace *workspace = initializedWorkspace();
        QVERIFY(workspace != nullptr);
        const QPointer<TerminalWorkspace> guard(workspace);
        connect(workspace->tabModel(), &QAbstractItemModel::rowsInserted,
                this, [workspace] { delete workspace; });
        workspace->newTab();
        QVERIFY(guard.isNull());
    }

    {
        TerminalWorkspace *workspace = initializedWorkspace();
        QVERIFY(workspace != nullptr);
        const QPointer<TerminalWorkspace> guard(workspace);
        connect(workspace, &TerminalWorkspace::tabTitlesChanged,
                this, [workspace] { delete workspace; });
        workspace->newTab();
        QVERIFY(guard.isNull());
    }

    {
        TerminalWorkspace *workspace = initializedWorkspace();
        QVERIFY(workspace != nullptr);
        const QPointer<TerminalWorkspace> guard(workspace);
        connect(workspace, &TerminalWorkspace::tabBarVisibleChanged,
                this, [workspace] { delete workspace; });
        workspace->newTab();
        QVERIFY(guard.isNull());
    }
}

void TerminalWorkspaceTest::deferredInitialSessionCancelsAndScopesToFirstPane()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    const QString temporaryRoot =
        QDir::current().filePath(QStringLiteral("tmp"));
    QVERIFY(QDir().mkpath(temporaryRoot));
    QTemporaryDir temporary(
        QDir(temporaryRoot)
            .filePath(QStringLiteral("terminal-workspace-deferred-XXXXXX")));
    QVERIFY(temporary.isValid());
    const QString startMarker =
        temporary.filePath(QStringLiteral("unexpected-start"));
    LaunchOptions markerOptions = options;
    markerOptions.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("printf started > \"$0\""), startMarker,
    };
    {
        QQuickWindow window;
        window.resize(640, 360);
        auto *workspace = new TerminalWorkspace(window.contentItem());
        workspace->setParentItem(window.contentItem());
        workspace->setSize(window.size());
        QVERIFY(workspace->initialize(
            markerOptions, TerminalSessionStartMode::Deferred));
        TerminalController *const controller =
            workspace->findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        QVERIFY(!workspace->closeAssessment().needsConfirmation);
        QVERIFY(workspace->armInitialSessionStart());
        controller->sendRawText(QByteArrayLiteral("must-not-run\n"));
        QPointer<TerminalWorkspace> guardedWorkspace(workspace);
        QPointer<TerminalController> guardedController(controller);
        connect(workspace, &TerminalWorkspace::windowCloseApproved,
                &window, [workspace] { delete workspace; });
        workspace->requestWindowClose();
        QTRY_VERIFY_WITH_TIMEOUT(guardedWorkspace.isNull(), 1000);
        QVERIFY(guardedController.isNull());

        window.showMaximized();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
        QTest::qWait(25);
        QVERIFY(!QFileInfo::exists(startMarker));
    }

    {
        QQuickWindow window;
        window.resize(640, 360);
        auto *workspace = new TerminalWorkspace(window.contentItem());
        workspace->setParentItem(window.contentItem());
        workspace->setSize(window.size());
        QVERIFY(workspace->initialize(
            options, TerminalSessionStartMode::Deferred));
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        QPointer<TerminalWorkspace> guardedWorkspace(workspace);
        QPointer<TerminalPane> guardedPane(pane);
        connect(pane, &TerminalPane::processStateChanged,
                &window, [workspace] { delete workspace; },
                Qt::SingleShotConnection);
        QVERIFY(workspace->armInitialSessionStart());
        window.showMaximized();
        QTRY_VERIFY_WITH_TIMEOUT(guardedWorkspace.isNull(), 1000);
        QVERIFY(guardedPane.isNull());
    }

    {
        QQuickWindow window;
        window.resize(640, 360);
        auto *workspace = new TerminalWorkspace(window.contentItem());
        workspace->setParentItem(window.contentItem());
        workspace->setSize(window.size());
        QVERIFY(workspace->initialize(
            options, TerminalSessionStartMode::Deferred));
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        TerminalController *const controller =
            pane->findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        QVERIFY(!controller->sessionStarted());
        QVERIFY(!controller->running());
        QVERIFY(!controller->activeProcess());
        QVERIFY(controller->findChild<QThread *>() == nullptr);
        QVERIFY(!workspace->closeAssessment().needsConfirmation);
        QVERIFY(!pane->resizeOverlayVisible());

        window.showMaximized();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
        pane->update();
        QVERIFY(!window.grabWindow().isNull());
        const TerminalPaneRenderProbeSnapshot deferredProbe =
            terminalPaneRenderProbe(pane);
        QVERIFY(deferredProbe.rootSerial != 0);
        QCOMPARE(deferredProbe.backgroundImageAssetSerial, quint64{0});
        QVERIFY(deferredProbe.backgroundImageRect.isEmpty());
        QVERIFY(deferredProbe.backgroundImageSourceRect.isEmpty());
        QVERIFY(!pane->resizeOverlayVisible());
        QVERIFY(!controller->sessionStarted());
        QVERIFY(controller->findChild<QThread *>() == nullptr);

        pane->setSize(pane->size() + QSizeF(7.0, 5.0));
        QCoreApplication::processEvents();
        QVERIFY(!pane->resizeOverlayVisible());
        QVERIFY(!controller->sessionStarted());
        QVERIFY(controller->findChild<QThread *>() == nullptr);

        QVERIFY(workspace->armInitialSessionStart());
        pane->beginShutdown();
        QTest::qWait(25);
        QVERIFY(!controller->sessionStarted());
        QVERIFY(controller->findChild<QThread *>() == nullptr);
        QVERIFY(!workspace->closeAssessment().needsConfirmation);
    }

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(640.0, 360.0));
    QVERIFY(workspace.initialize(
        options, TerminalSessionStartMode::Deferred));
    const CurrentTabProbe first = currentTabProbe(workspace);
    QVERIFY(first.pane != nullptr);
    TerminalController *const firstController =
        first.pane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QVERIFY(!firstController->sessionStarted());

    workspace.newTab();
    const CurrentTabProbe second = currentTabProbe(workspace);
    QVERIFY(second.pane != nullptr);
    QVERIFY(second.pane != first.pane);
    TerminalController *const secondController =
        second.pane->findChild<TerminalController *>();
    QVERIFY(secondController != nullptr);
    QVERIFY(secondController->sessionStarted());

    const CurrentTabProbe third = splitRightProbe(workspace);
    QVERIFY(third.pane != nullptr);
    TerminalController *const thirdController =
        third.pane->findChild<TerminalController *>();
    QVERIFY(thirdController != nullptr);
    QVERIFY(thirdController->sessionStarted());
    QVERIFY(!firstController->sessionStarted());
}

void TerminalWorkspaceTest::
keybindProgramStorageIsSharedWhilePaneStateIsLocal()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;

    const auto unicode = [](quint32 codepoint, quint8 modifiers = 0) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = modifiers,
        };
    };
    GhosttyKeybindConfig config;
    config.tables = {GhosttyKeybindTable{
        .name = QStringLiteral("modal"),
        .bindings = {GhosttyKeybindDefinition{
            .sequence = {unicode('r', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("ignore")},
        }},
    }};
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));
    const GhosttyKeybindProgram firstProgram =
        GhosttyKeybindProgram::compile(options.keybindSource).program;
    QVERIFY(firstProgram.isAvailable());

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(640.0, 360.0));
    QVERIFY(workspace.initialize(
        options, TerminalSessionStartMode::Deferred, {}, firstProgram));
    QVERIFY(workspace.keybindProgram().isSameGeneration(firstProgram));

    const CurrentTabProbe initial = currentTabProbe(workspace);
    QVERIFY(initial.pane != nullptr);
    workspace.newTab();
    const CurrentTabProbe laterTab = currentTabProbe(workspace);
    const CurrentTabProbe laterSplit = splitRightProbe(workspace);
    QVERIFY(laterTab.pane != nullptr);
    QVERIFY(laterSplit.pane != nullptr);

    const QList<TerminalPane *> firstGenerationPanes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(firstGenerationPanes.size(), 3);
    for (TerminalPane *pane : firstGenerationPanes) {
        QVERIFY(pane->keybindProgram().isSameGeneration(firstProgram));
        QVERIFY(pane->keybindProgram().isSameGeneration(
            workspace.keybindProgram()));
    }

    // The immutable program is shared, but named-table state belongs to the
    // pane that activated it. The same trigger therefore resolves only on
    // that pane.
    QVERIFY(initial.pane->executeConfiguredAction(
        QStringLiteral("activate_key_table:modal")));
    QCOMPARE(initial.pane->activeKeyTables(),
             QStringList({QStringLiteral("modal")}));
    QVERIFY(laterTab.pane->activeKeyTables().isEmpty());
    QVERIFY(laterSplit.pane->activeKeyTables().isEmpty());

    LaunchOptions reloadedOptions = options;
    GhosttyKeybindConfig reloadedConfig;
    reloadedConfig.root = {GhosttyKeybindDefinition{
        .sequence = {unicode('z', GhosttyKeybindCtrl)},
        .actions = {QStringLiteral("reset_font_size")},
    }};
    reloadedOptions.keybindSource =
        GhosttyKeybindSource::structured(std::move(reloadedConfig));
    const GhosttyKeybindProgram reloadedProgram =
        GhosttyKeybindProgram::compile(reloadedOptions.keybindSource).program;
    QVERIFY(!reloadedProgram.isSameGeneration(firstProgram));

    workspace.applyLaunchOptions(reloadedOptions, reloadedProgram);
    QVERIFY(workspace.keybindProgram().isSameGeneration(reloadedProgram));
    QVERIFY(!workspace.keybindProgram().isSameGeneration(firstProgram));
    for (TerminalPane *pane : firstGenerationPanes) {
        QVERIFY(pane->keybindProgram().isSameGeneration(reloadedProgram));
        QVERIFY(!pane->keybindProgram().isSameGeneration(firstProgram));
    }
    QVERIFY(initial.pane->activeKeyTables().isEmpty());

    workspace.newTab();
    const CurrentTabProbe reloadedTab = currentTabProbe(workspace);
    const CurrentTabProbe reloadedSplit = splitRightProbe(workspace);
    QVERIFY(reloadedTab.pane != nullptr);
    QVERIFY(reloadedSplit.pane != nullptr);

    const QList<TerminalPane *> reloadedPanes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(reloadedPanes.size(), 5);
    for (TerminalPane *pane : reloadedPanes) {
        QVERIFY(pane->keybindProgram().isSameGeneration(reloadedProgram));
        QVERIFY(pane->keybindProgram().isSameGeneration(
            workspace.keybindProgram()));
        QVERIFY(!pane->keybindProgram().isSameGeneration(firstProgram));
    }
}

void TerminalWorkspaceTest::newerSameProgramWorkspaceUpdateWinsReentry()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.windowShowTabBar = WindowShowTabBar::Auto;
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(options.keybindSource).program;

    TerminalWorkspace workspace;
    QVERIFY(workspace.initialize(
        options, TerminalSessionStartMode::Deferred, {}, program));
    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    QVERIFY(workspace.tabBarVisible());

    LaunchOptions outer = options;
    outer.typography.pointSize = 14.0;
    outer.windowShowTabBar = WindowShowTabBar::Never;
    LaunchOptions newer = options;
    newer.typography.pointSize = 19.0;
    newer.windowShowTabBar = WindowShowTabBar::Always;

    bool nested = false;
    connect(&workspace, &TerminalWorkspace::tabBarVisibleChanged,
            &workspace, [&] {
                if (nested) return;
                nested = true;
                workspace.applyLaunchOptions(newer, program);
            });

    workspace.applyLaunchOptions(outer, program);

    QVERIFY(nested);
    QCOMPARE(workspace.effectiveLaunchOptions(), newer);
    QVERIFY(workspace.keybindProgram().isSameGeneration(program));
    const QList<TerminalPane *> panes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 2);
    for (const TerminalPane *pane : panes) {
        QVERIFY(pane->keybindProgram().isSameGeneration(program));
        QCOMPARE(pane->fontPointSize(), newer.typography.pointSize);
    }
}

void TerminalWorkspaceTest::runningProgramPromptsThenResolvesOnceOnExit()
{
    LaunchOptions options = baseOptions();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 0.6"),
    };
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy resolved(&workspace,
                        &TerminalWorkspace::closeConfirmationResolved);
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

    workspace.requestWindowClose();
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(quit.count(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(resolved.count(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 3000);
    QTest::qWait(100);
    QCOMPARE(quit.count(), 1);
}

void TerminalWorkspaceTest::distinguishesWindowCloseFromApplicationQuit()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    {
        TerminalWorkspace workspace;
        QSignalSpy windowClose(
            &workspace, &TerminalWorkspace::windowCloseApproved);
        QSignalSpy applicationQuit(
            &workspace, &TerminalWorkspace::applicationQuitApproved);
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

        workspace.requestWindowClose();
        workspace.requestWindowClose();
        QCOMPARE(windowClose.count(), 0);
        QCOMPARE(applicationQuit.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(windowClose.count(), 1, 1000);
        QCOMPARE(applicationQuit.count(), 0);
    }

    {
        TerminalWorkspace workspace;
        QSignalSpy windowClose(
            &workspace, &TerminalWorkspace::windowCloseApproved);
        QSignalSpy applicationQuit(
            &workspace, &TerminalWorkspace::applicationQuitApproved);
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        TerminalPane *const pane = workspace.findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);

        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("close_window")));
        QTRY_COMPARE_WITH_TIMEOUT(windowClose.count(), 1, 1000);
        QCOMPARE(applicationQuit.count(), 0);
    }

    {
        TerminalWorkspace workspace;
        QSignalSpy windowClose(
            &workspace, &TerminalWorkspace::windowCloseApproved);
        QSignalSpy applicationQuit(
            &workspace, &TerminalWorkspace::applicationQuitApproved);
        QStringList publicationOrder;
        connect(&workspace, &TerminalWorkspace::windowCloseApproved,
                &workspace, [&publicationOrder] {
                    publicationOrder.append(QStringLiteral("window"));
                });
        connect(&workspace, &TerminalWorkspace::applicationQuitApproved,
                &workspace, [&publicationOrder] {
                    publicationOrder.append(QStringLiteral("application"));
                });
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

        workspace.requestApplicationQuitConfirmation(
            workspace.closeAssessment());
        workspace.requestApplicationQuitConfirmation(
            workspace.closeAssessment());
        QCOMPARE(windowClose.count(), 0);
        QCOMPARE(applicationQuit.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(windowClose.count(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(applicationQuit.count(), 1, 1000);
        QCOMPARE(publicationOrder,
                 QStringList({QStringLiteral("window"),
                              QStringLiteral("application")}));
    }

    options.confirmCloseMode = ConfirmCloseMode::Always;
    TerminalWorkspace::setDefaultLaunchOptions(options);
    TerminalWorkspace workspace;
    QSignalSpy confirmation(
        &workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy windowClose(
        &workspace, &TerminalWorkspace::windowCloseApproved);
    QSignalSpy applicationQuit(
        &workspace, &TerminalWorkspace::applicationQuitApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *const pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    workspace.requestApplicationQuitConfirmation(workspace.closeAssessment());
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(windowClose.count(), 0);
    QCOMPARE(applicationQuit.count(), 0);
    workspace.cancelClose(closeConfirmationId(confirmation));
    QCOMPARE(windowClose.count(), 0);
    QCOMPARE(applicationQuit.count(), 0);

    workspace.requestApplicationQuitConfirmation(workspace.closeAssessment());
    QCOMPARE(confirmation.count(), 2);
    workspace.confirmClose(closeConfirmationId(confirmation));
    QCOMPARE(windowClose.count(), 0);
    QCOMPARE(applicationQuit.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(windowClose.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(applicationQuit.count(), 1, 1000);
}

void TerminalWorkspaceTest::configuredCloseChainSurvivesSynchronousWorkspaceDestruction()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+k=close_window"),
        QStringLiteral("chain=new_tab"),
    });
    TerminalWorkspace::setDefaultLaunchOptions(options);

    auto *workspace = new TerminalWorkspace;
    const QPointer<TerminalWorkspace> guardedWorkspace(workspace);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
    TerminalPane *const pane = workspace->findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);

    int approvals = 0;
    connect(workspace, &TerminalWorkspace::windowCloseApproved,
            [&approvals, workspace] {
                ++approvals;
                delete workspace;
            });

    QKeyEvent press(QEvent::KeyPress, Qt::Key_K,
                    Qt::ControlModifier, QString(QChar(0x0b)));
    QCoreApplication::sendEvent(pane, &press);

    // The later action runs before the lifecycle owner commits the close.
    // The approval observer may then destroy the entire QObject hierarchy
    // synchronously without returning into a half-executed pane chain.
    QVERIFY(!guardedWorkspace.isNull());
    QCOMPARE(guardedWorkspace->tabCount(), 2);
    QCOMPARE(approvals, 0);
    QTRY_VERIFY_WITH_TIMEOUT(guardedWorkspace.isNull(), 1000);
    QCOMPARE(approvals, 1);
}

void TerminalWorkspaceTest::
configuredSurfaceCloseChainSurvivesSynchronousDestruction_data()
{
    QTest::addColumn<QString>("action");

    QTest::newRow("surface") << QStringLiteral("close_surface");
    QTest::newRow("tab") << QStringLiteral("close_tab:this");
}

void TerminalWorkspaceTest::
configuredSurfaceCloseChainSurvivesSynchronousDestruction()
{
    QFETCH(QString, action);

    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+k=%1").arg(action),
        QStringLiteral("chain=increase_font_size:1"),
    });
    TerminalWorkspace::setDefaultLaunchOptions(options);

    auto *workspace = new TerminalWorkspace;
    const QPointer<TerminalWorkspace> guardedWorkspace(workspace);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
    TerminalPane *const pane = workspace->findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    const QPointer<TerminalPane> guardedPane(pane);
    TerminalController *const controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    const qreal initialFontSize = pane->fontPointSize();

    int approvals = 0;
    connect(workspace, &TerminalWorkspace::windowCloseApproved,
            this, [&approvals, workspace] {
                ++approvals;
                delete workspace;
            });

    sendCtrlKPressAndRelease(pane);

    // Closing the final surface commits synchronously, but the still-live
    // pane completes the chain and key bookkeeping before approval is
    // published to a potentially destructive host.
    QVERIFY(guardedWorkspace != nullptr);
    QVERIFY(guardedPane != nullptr);
    QCOMPARE(guardedWorkspace->tabCount(), 0);
    QCOMPARE(guardedPane->fontPointSize(), initialFontSize + 1.0);
    QCOMPARE(approvals, 0);
    QCOMPARE(forwarded.count(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(guardedWorkspace.isNull(), 1000);
    QVERIFY(guardedPane.isNull());
    QCOMPARE(approvals, 1);
}

void TerminalWorkspaceTest::
broadCloseChainSurvivesSynchronousSourceDestruction_data()
{
    QTest::addColumn<bool>("global");

    QTest::newRow("all") << false;
    QTest::newRow("global") << true;
}

void TerminalWorkspaceTest::
broadCloseChainSurvivesSynchronousSourceDestruction()
{
    QFETCH(bool, global);

    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    GhosttyKeybindConfig config;
    config.root = {GhosttyKeybindDefinition{
        .sequence = {GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = 'k',
            .modifiers = GhosttyKeybindCtrl,
        }},
        .actions = {
            QStringLiteral("close_surface"),
            QStringLiteral("reload_config"),
        },
        .flags = global
            ? GhosttyKeybindFlags{.global = true}
            : GhosttyKeybindFlags{.all = true},
    }};
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));
    TerminalWorkspace::setDefaultLaunchOptions(options);

    auto *workspace = new TerminalWorkspace;
    GhosttyApplicationKeybindings bindings(options, false);
    bindings.registerWorkspace(workspace);
    const QPointer<TerminalWorkspace> guardedWorkspace(workspace);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
    TerminalPane *const pane = workspace->findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *const controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy reload(
        &bindings,
        &GhosttyApplicationKeybindings::applicationActionRequested);

    int approvals = 0;
    connect(workspace, &TerminalWorkspace::windowCloseApproved,
            this, [&approvals, workspace] {
                ++approvals;
                delete workspace;
            });

    sendCtrlKPressAndRelease(pane);

    QVERIFY(guardedWorkspace != nullptr);
    QCOMPARE(reload.count(), 1);
    QCOMPARE(qvariant_cast<ApplicationAction>(
                 reload.constFirst().constFirst()),
             ApplicationAction::ReloadConfig);
    QCOMPARE(approvals, 0);
    QCOMPARE(forwarded.count(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(guardedWorkspace.isNull(), 1000);
    QCOMPARE(approvals, 1);
}

void TerminalWorkspaceTest::allPaneClosePublicationOutlivesFanout()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    auto *workspace = new TerminalWorkspace;
    const QPointer<TerminalWorkspace> guardedWorkspace(workspace);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
    workspace->splitRight();
    workspace->newTab();
    QCOMPARE(workspace->findChildren<TerminalPane *>().size(), 3);

    int approvals = 0;
    connect(workspace, &TerminalWorkspace::windowCloseApproved,
            this, [&approvals, workspace] {
                ++approvals;
                delete workspace;
            });

    QVERIFY(workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("close_surface")));
    QVERIFY(guardedWorkspace != nullptr);
    QCOMPARE(guardedWorkspace->tabCount(), 0);
    QCOMPARE(approvals, 0);

    QTRY_VERIFY_WITH_TIMEOUT(guardedWorkspace.isNull(), 1000);
    QCOMPARE(approvals, 1);
}

void TerminalWorkspaceTest::
broadFanoutSurvivesSynchronousWorkspaceDestruction()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    auto *workspace = new TerminalWorkspace;
    const QPointer<TerminalWorkspace> guardedWorkspace(workspace);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
    workspace->splitRight();
    workspace->newTab();
    const QList<TerminalPane *> panes =
        workspace->findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 3);

    std::vector<QPointer<TerminalPane>> guardedPanes;
    guardedPanes.reserve(static_cast<std::size_t>(panes.size()));
    int destructions = 0;
    for (TerminalPane *pane : panes) {
        guardedPanes.emplace_back(pane);
        connect(pane, &TerminalPane::fontPointSizeChanged,
                this, [&, workspace] {
                    if (guardedWorkspace == nullptr) return;
                    ++destructions;
                    delete workspace;
                });
    }

    QVERIFY(workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("increase_font_size:1")));
    QVERIFY(guardedWorkspace.isNull());
    QCOMPARE(destructions, 1);
    QVERIFY(std::ranges::all_of(
        guardedPanes, [](const auto &pane) { return pane.isNull(); }));
}

void TerminalWorkspaceTest::
unexpectedDirectObserverDestructionStopsChainSafely()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+k=new_tab"),
        QStringLiteral("chain=toggle_readonly"),
    });

    auto *pane = new TerminalPane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    const QPointer<TerminalPane> guardedPane(pane);
    int readOnlyChanges = 0;
    connect(pane, &TerminalPane::readOnlyChanged,
            this, [&readOnlyChanges] { ++readOnlyChanges; });
    pane->setWorkspaceActionHandler(
        [pane](WorkspaceActionRequest) {
            delete pane;
            return true;
        });

    QKeyEvent press(QEvent::KeyPress, Qt::Key_K,
                    Qt::ControlModifier, QString(QChar(0x0b)));
    QCoreApplication::sendEvent(pane, &press);

    QVERIFY(guardedPane.isNull());
    QCOMPARE(readOnlyChanges, 0);
}

void TerminalWorkspaceTest::synchronousObserversMayDestroyWorkspace()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    {
        options.confirmCloseMode = ConfirmCloseMode::Always;
        TerminalWorkspace::setDefaultLaunchOptions(options);
        auto *workspace = new TerminalWorkspace;
        const QPointer<TerminalWorkspace> guard(workspace);
        QSignalSpy confirmation(
            workspace, &TerminalWorkspace::closeConfirmationRequested);
        QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
        workspace->requestApplicationQuitConfirmation(
            workspace->closeAssessment());
        QCOMPARE(confirmation.count(), 1);
        connect(workspace, &TerminalWorkspace::closeConfirmationResolved,
                this, [workspace] { delete workspace; });

        workspace->cancelClose(closeConfirmationId(confirmation));

        QVERIFY(guard.isNull());
    }

    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);
    {
        auto *workspace = new TerminalWorkspace;
        const QPointer<TerminalWorkspace> guard(workspace);
        QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("prompt_surface_title")));
        connect(workspace, &TerminalWorkspace::titlePromptResolved,
                this, [workspace] { delete workspace; });

        workspace->closeCurrentTab();

        QVERIFY(guard.isNull());
    }

    {
        auto *workspace = new TerminalWorkspace;
        const QPointer<TerminalWorkspace> guard(workspace);
        QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        QSignalSpy confirmation(
            workspace,
            &TerminalWorkspace::unsafePasteConfirmationRequested);
        Q_EMIT pane->unsafePasteRequested(
            1, QStringLiteral("unsafe\ntext"), pane);
        QCOMPARE(confirmation.count(), 1);
        connect(
            workspace,
            &TerminalWorkspace::unsafePasteConfirmationResolved,
            this, [workspace] { delete workspace; });

        workspace->closeCurrentTab();

        QVERIFY(guard.isNull());
    }

    {
        auto *workspace = new TerminalWorkspace;
        const QPointer<TerminalWorkspace> guard(workspace);
        QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
        workspace->newTab();
        workspace->newTab();
        QCOMPARE(workspace->tabCount(), 3);
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        int removals = 0;
        connect(
            workspace->tabModel(), &QAbstractItemModel::rowsRemoved,
            this, [&removals, workspace] {
                ++removals;
                delete workspace;
            });

        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("close_tab:other")));

        QVERIFY(guard.isNull());
        QCOMPARE(removals, 1);
    }

    {
        auto *workspace = new TerminalWorkspace;
        const QPointer<TerminalWorkspace> guard(workspace);
        QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
        TerminalPane *const pane = workspace->findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        Q_EMIT pane->contextMenuRequested(QPointF(20.0, 30.0), true);
        connect(workspace, &TerminalWorkspace::contextMenuCancelled, this,
                [workspace] { delete workspace; });

        workspace->closeCurrentTab();

        QVERIFY(guard.isNull());
    }
}

void TerminalWorkspaceTest::contextMenuRequestsUseStablePaneTargets()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(900, 600);
    TerminalWorkspace workspace(window.contentItem());
    workspace.setSize(window.size());
    window.show();
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

    const CurrentTabProbe first = currentTabProbe(workspace);
    const CurrentTabProbe second = splitRightProbe(workspace);
    QVERIFY(first.pane != nullptr);
    QVERIFY(second.pane != nullptr);
    TerminalController *const firstController =
        first.pane->findChild<TerminalController *>();
    TerminalController *const secondController =
        second.pane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QVERIFY(secondController != nullptr);
    QSignalSpy firstCopies(firstController,
                           &TerminalController::copyActionRequested);
    QSignalSpy secondCopies(secondController,
                            &TerminalController::copyActionRequested);
    QSignalSpy firstPastes(firstController,
                           &TerminalController::pasteRequested);
    QSignalSpy secondPastes(secondController,
                            &TerminalController::pasteRequested);
    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const QString previousClipboardText = clipboard->text();
    const auto restoreClipboard =
        qScopeGuard([clipboard, previousClipboardText] {
            clipboard->setText(previousClipboardText);
        });
    const QString pasteText = QStringLiteral("context-menu-stable-target");
    clipboard->setText(pasteText);

    QSignalSpy requests(&workspace, &TerminalWorkspace::contextMenuRequested);
    QSignalSpy cancellations(&workspace,
                             &TerminalWorkspace::contextMenuCancelled);
    const QPointF windowPosition(123.5, 87.25);
    Q_EMIT first.pane->contextMenuRequested(windowPosition, true);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.constFirst().at(1).toPointF(), windowPosition);
    QCOMPARE(requests.constFirst().at(2).toBool(), true);
    const quint64 firstRequestId =
        requests.constFirst().constFirst().toULongLong();
    QVERIFY(firstRequestId != 0);

    QVERIFY(!workspace.executeContextMenuAction(
        firstRequestId + 1, QStringLiteral("copy_to_clipboard:mixed")));
    QVERIFY(!workspace.executeContextMenuAction(
        firstRequestId, QStringLiteral("copy_to_clipboard")));
    QVERIFY(!workspace.executeContextMenuAction(
        firstRequestId, QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(workspace.executeContextMenuAction(
        firstRequestId, QStringLiteral("copy_to_clipboard:mixed")));
    QCOMPARE(firstCopies.count(), 1);
    QCOMPARE(secondCopies.count(), 0);
    QVERIFY(!workspace.executeContextMenuAction(
        firstRequestId, QStringLiteral("paste_from_clipboard")));

    workspace.finishContextMenu(firstRequestId + 1);
    Q_EMIT first.pane->contextMenuRequested(windowPosition, false);
    QCOMPARE(requests.count(), 2);
    const quint64 pasteRequestId =
        requests.constLast().constFirst().toULongLong();
    QVERIFY(workspace.executeContextMenuAction(
        pasteRequestId, QStringLiteral("paste_from_clipboard")));
    QCOMPARE(firstPastes.count(), 1);
    QCOMPARE(firstPastes.constFirst().constFirst().toString(), pasteText);
    QCOMPARE(secondPastes.count(), 0);

    Q_EMIT first.pane->contextMenuRequested(windowPosition, false);
    QCOMPARE(requests.count(), 3);
    const quint64 dismissalRequestId =
        requests.constLast().constFirst().toULongLong();
    QQuickItem focusSink(window.contentItem());
    focusSink.forceActiveFocus();
    QCOMPARE(window.activeFocusItem(), &focusSink);
    QVERIFY(second.pane != nullptr);
    workspace.finishContextMenu(dismissalRequestId + 1);
    QCOMPARE(window.activeFocusItem(), &focusSink);
    workspace.finishContextMenu(dismissalRequestId);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), second.pane.data(),
                              1000);
    QVERIFY(!workspace.executeContextMenuAction(
        dismissalRequestId, QStringLiteral("copy_to_clipboard:mixed")));

    // A synchronous cancellation observer may publish a still-newer menu.
    // That nested request wins; the outer superseding request must not
    // overwrite it without a matching cancellation.
    const QPointF supersededPosition(210.0, 110.0);
    const QPointF nestedPosition(220.0, 120.0);
    Q_EMIT first.pane->contextMenuRequested(supersededPosition, false);
    QCOMPARE(requests.count(), 4);
    connect(
        &workspace, &TerminalWorkspace::contextMenuCancelled, &workspace,
        [pane = first.pane, nestedPosition] {
            if (pane != nullptr) {
                Q_EMIT pane->contextMenuRequested(nestedPosition, true);
            }
        },
        Qt::SingleShotConnection);
    Q_EMIT second.pane->contextMenuRequested(QPointF(230.0, 130.0), false);
    QCOMPARE(cancellations.count(), 1);
    QCOMPARE(requests.count(), 5);
    QCOMPARE(requests.constLast().at(1).toPointF(), nestedPosition);
    const quint64 nestedRequestId =
        requests.constLast().constFirst().toULongLong();
    QVERIFY(workspace.executeContextMenuAction(
        nestedRequestId, QStringLiteral("copy_to_clipboard:mixed")));
    QCOMPARE(firstCopies.count(), 2);
    QCOMPARE(secondCopies.count(), 0);
    workspace.finishContextMenu(nestedRequestId);

    Q_EMIT first.pane->contextMenuRequested(windowPosition, false);
    QCOMPARE(requests.count(), 6);
    QCOMPARE(requests.constLast().at(2).toBool(), false);
    const quint64 removalRequestId =
        requests.constLast().constFirst().toULongLong();
    QVERIFY(removalRequestId != 0);
    QVERIFY(removalRequestId != firstRequestId);

    QPointer<TerminalPane> removedPane(first.pane);
    connect(
        &workspace, &TerminalWorkspace::contextMenuCancelled, &workspace,
        [pane = first.pane] {
            if (pane != nullptr) {
                Q_EMIT pane->contextMenuRequested(QPointF(240.0, 140.0), false);
            }
        },
        Qt::SingleShotConnection);
    QVERIFY(
        first.pane->executeConfiguredAction(QStringLiteral("close_surface")));
    QCOMPARE(cancellations.count(), 2);
    QCOMPARE(cancellations.constLast().constFirst().toULongLong(),
             removalRequestId);
    QCOMPARE(requests.count(), 6);
    QVERIFY(!workspace.executeContextMenuAction(
        removalRequestId, QStringLiteral("paste_from_clipboard")));
    QTRY_VERIFY_WITH_TIMEOUT(removedPane.isNull(), 1000);
}

void TerminalWorkspaceTest::contextMenuRoutesSurfaceAndSplitActions()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(900, 600);
    TerminalWorkspace workspace(window.contentItem());
    workspace.setSize(window.size());
    window.show();
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

    const CurrentTabProbe first = currentTabProbe(workspace);
    const CurrentTabProbe second = splitRightProbe(workspace);
    QVERIFY(first.pane != nullptr);
    QVERIFY(second.pane != nullptr);
    TerminalController *const firstController =
        first.pane->findChild<TerminalController *>();
    TerminalController *const secondController =
        second.pane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QVERIFY(secondController != nullptr);

    QSignalSpy firstResets(firstController,
                           &TerminalController::resetTerminalRequested);
    QSignalSpy secondResets(secondController,
                            &TerminalController::resetTerminalRequested);
    QSignalSpy requests(&workspace, &TerminalWorkspace::contextMenuRequested);
    QSignalSpy cancellations(&workspace,
                             &TerminalWorkspace::contextMenuCancelled);
    QSignalSpy titlePrompts(&workspace,
                            &TerminalWorkspace::titlePromptRequested);
    const auto requestForFirst = [&] {
        const int priorCount = requests.count();
        Q_EMIT first.pane->contextMenuRequested(QPointF(130.0, 90.0), false);
        if (requests.count() != priorCount + 1) return quint64{0};
        return requests.constLast().constFirst().toULongLong();
    };

    const quint64 resetRequestId = requestForFirst();
    QVERIFY(resetRequestId != 0);
    const std::array<QString, 5> rejectedActions{
        QStringLiteral("new_split"),
        QStringLiteral("new_split:auto"),
        QStringLiteral("reset:"),
        QStringLiteral("set_surface_title:menu"),
        QStringLiteral("clear_screen"),
    };
    for (const QString &action : rejectedActions) {
        QVERIFY(!workspace.executeContextMenuAction(resetRequestId, action));
    }
    QVERIFY(workspace.executeContextMenuAction(resetRequestId,
                                               QStringLiteral("reset")));
    QCOMPARE(firstResets.count(), 1);
    QCOMPARE(secondResets.count(), 0);
    QVERIFY(!workspace.executeContextMenuAction(resetRequestId,
                                                QStringLiteral("reset")));

    QQuickItem focusSink(window.contentItem());
    focusSink.forceActiveFocus();
    QCOMPARE(window.activeFocusItem(), &focusSink);
    bool activePaneFocusedWhenPrompted = false;
    connect(&workspace, &TerminalWorkspace::titlePromptRequested, &workspace,
            [&window, &second, &activePaneFocusedWhenPrompted] {
                activePaneFocusedWhenPrompted =
                    window.activeFocusItem() == second.pane.data();
            });

    const quint64 titleRequestId = requestForFirst();
    QVERIFY(titleRequestId != 0);
    QVERIFY(workspace.executeContextMenuAction(
        titleRequestId, QStringLiteral("prompt_surface_title")));
    QCOMPARE(titlePrompts.count(), 1);
    QVERIFY(activePaneFocusedWhenPrompted);
    const quint64 titlePromptId =
        titlePrompts.constFirst().constFirst().toULongLong();
    const QString title = QStringLiteral("context-menu-origin");
    workspace.confirmTitlePrompt(titlePromptId, title);
    QCOMPARE(first.pane->surfaceTitleOverride(), std::optional<QString>{title});
    QVERIFY(!second.pane->surfaceTitleOverride().has_value());

    focusSink.forceActiveFocus();
    QCOMPARE(window.activeFocusItem(), &focusSink);
    workspace.finishContextMenu(titleRequestId);
    QCOMPARE(window.activeFocusItem(), &focusSink);
    QVERIFY(!workspace.executeContextMenuAction(titleRequestId,
                                                QStringLiteral("reset")));

    struct DirectionCase {
        QString action;
        QPointF expectedSign;
    };
    const std::array<DirectionCase, 4> directions{{
        {QStringLiteral("new_split:up"), QPointF(0.0, -1.0)},
        {QStringLiteral("new_split:down"), QPointF(0.0, 1.0)},
        {QStringLiteral("new_split:left"), QPointF(-1.0, 0.0)},
        {QStringLiteral("new_split:right"), QPointF(1.0, 0.0)},
    }};
    for (const DirectionCase &direction : directions) {
        const QList<TerminalPane *> panesBefore =
            workspace.findChildren<TerminalPane *>();
        const quint64 requestId = requestForFirst();
        QVERIFY(requestId != 0);
        QVERIFY(
            workspace.executeContextMenuAction(requestId, direction.action));
        QCOMPARE(workspace.findChildren<TerminalPane *>().size(),
                 panesBefore.size() + 1);

        TerminalPane *addedPane = nullptr;
        for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
            if (!panesBefore.contains(pane)) {
                QVERIFY(addedPane == nullptr);
                addedPane = pane;
            }
        }
        QVERIFY(addedPane != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            addedPane->width() > 0.0 && addedPane->height() > 0.0
                && first.pane->width() > 0.0 && first.pane->height() > 0.0,
            1000);
        const QPointF sourceCenter = first.pane->mapToItem(
            &workspace,
            QPointF(first.pane->width() / 2.0, first.pane->height() / 2.0));
        const QPointF addedCenter = addedPane->mapToItem(
            &workspace,
            QPointF(addedPane->width() / 2.0, addedPane->height() / 2.0));
        if (!qFuzzyIsNull(direction.expectedSign.x())) {
            QVERIFY((addedCenter.x() - sourceCenter.x())
                        * direction.expectedSign.x()
                    > 0.0);
        } else {
            QVERIFY((addedCenter.y() - sourceCenter.y())
                        * direction.expectedSign.y()
                    > 0.0);
        }
        QVERIFY(!workspace.executeContextMenuAction(requestId,
                                                    QStringLiteral("reset")));
    }

    const int paneCountBeforeClose =
        workspace.findChildren<TerminalPane *>().size();
    const quint64 closeRequestId = requestForFirst();
    QVERIFY(closeRequestId != 0);
    QPointer<TerminalPane> removedPane(first.pane);
    QVERIFY(workspace.executeContextMenuAction(
        closeRequestId, QStringLiteral("close_surface")));
    QCOMPARE(cancellations.count(), 0);
    QVERIFY(!workspace.executeContextMenuAction(
        closeRequestId, QStringLiteral("close_surface")));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.findChildren<TerminalPane *>().size(),
                              paneCountBeforeClose - 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(removedPane.isNull(), 1000);
    QVERIFY(second.pane != nullptr);
}

void TerminalWorkspaceTest::contextMenuRoutesTabActions()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(900, 600);
    TerminalWorkspace workspace(window.contentItem());
    workspace.setSize(window.size());
    window.show();
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

    const CurrentTabProbe first = currentTabProbe(workspace);
    QVERIFY(first.pane != nullptr);
    QVERIFY(first.pane->executeConfiguredAction(
        QStringLiteral("set_tab_title:first menu seed")));

    QSignalSpy requests(&workspace, &TerminalWorkspace::contextMenuRequested);
    QSignalSpy cancellations(&workspace,
                             &TerminalWorkspace::contextMenuCancelled);
    QSignalSpy titlePrompts(&workspace,
                            &TerminalWorkspace::titlePromptRequested);
    const auto requestForFirst = [&] {
        const int priorCount = requests.count();
        Q_EMIT first.pane->contextMenuRequested(QPointF(150.0, 100.0), false);
        if (requests.count() != priorCount + 1) return quint64{0};
        return requests.constLast().constFirst().toULongLong();
    };

    const quint64 titleRequestId = requestForFirst();
    QVERIFY(titleRequestId != 0);
    const std::array<QString, 6> rejectedActions{
        QStringLiteral("prompt_tab_title:"), QStringLiteral("new_tab:"),
        QStringLiteral("close_tab"),         QStringLiteral("close_tab:"),
        QStringLiteral("close_tab:other"),   QStringLiteral("close_tab:right"),
    };
    for (const QString &action : rejectedActions) {
        QVERIFY(!workspace.executeContextMenuAction(titleRequestId, action));
    }

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    const CurrentTabProbe second = currentTabProbe(workspace);
    QVERIFY(second.pane != nullptr);
    QVERIFY(second.tabId != first.tabId);
    QCOMPARE(window.activeFocusItem(), second.pane.data());

    bool activePaneFocusedWhenPrompted = false;
    connect(&workspace, &TerminalWorkspace::titlePromptRequested, &workspace,
            [&window, &second, &activePaneFocusedWhenPrompted] {
                activePaneFocusedWhenPrompted =
                    window.activeFocusItem() == second.pane.data();
            });
    QVERIFY(workspace.executeContextMenuAction(
        titleRequestId, QStringLiteral("prompt_tab_title")));
    QCOMPARE(titlePrompts.count(), 1);
    QVERIFY(activePaneFocusedWhenPrompted);
    QCOMPARE(titlePrompts.constFirst().at(1).toString(),
             QStringLiteral("Change Tab Title"));
    QCOMPARE(titlePrompts.constFirst().at(2).toString(),
             QStringLiteral("first menu seed"));
    const quint64 titlePromptId =
        titlePrompts.constFirst().constFirst().toULongLong();
    const QString renamed = QStringLiteral("context-menu tab origin");
    workspace.confirmTitlePrompt(titlePromptId, renamed);
    const int firstIndex = workspace.tabModel()->indexOf(first.tabId);
    const int secondIndex = workspace.tabModel()->indexOf(second.tabId);
    QVERIFY(firstIndex >= 0);
    QVERIFY(secondIndex >= 0);
    QCOMPARE(workspace.tabModel()->entryAt(firstIndex)->titleOverride, renamed);
    QVERIFY(
        workspace.tabModel()->entryAt(secondIndex)->titleOverride.isEmpty());
    QVERIFY(!workspace.executeContextMenuAction(
        titleRequestId, QStringLiteral("prompt_tab_title")));

    QVERIFY(first.pane->executeConfiguredAction(
        QStringLiteral("increase_font_size:1")));
    QVERIFY(second.pane->executeConfiguredAction(
        QStringLiteral("increase_font_size:3")));
    const qreal firstFontSize = first.pane->fontPointSize();
    const qreal secondFontSize = second.pane->fontPointSize();
    QVERIFY(firstFontSize != secondFontSize);
    workspace.setCurrentIndex(workspace.tabModel()->indexOf(second.tabId));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             second.tabId);

    const int tabCountBeforeNew = workspace.tabCount();
    const quint64 newRequestId = requestForFirst();
    QVERIFY(newRequestId != 0);
    QVERIFY(workspace.executeContextMenuAction(newRequestId,
                                               QStringLiteral("new_tab")));
    QCOMPARE(workspace.tabCount(), tabCountBeforeNew + 1);
    const CurrentTabProbe created = currentTabProbe(workspace);
    QVERIFY(created.pane != nullptr);
    QVERIFY(created.tabId != first.tabId);
    QVERIFY(created.tabId != second.tabId);
    QCOMPARE(created.pane->fontPointSize(), firstFontSize);
    QVERIFY(created.pane->fontPointSize() != secondFontSize);
    QVERIFY(!workspace.executeContextMenuAction(newRequestId,
                                                QStringLiteral("new_tab")));

    const int tabCountBeforeClose = workspace.tabCount();
    const quint64 closeRequestId = requestForFirst();
    QVERIFY(closeRequestId != 0);
    QPointer<TerminalPane> removedPane(first.pane);
    QVERIFY(workspace.executeContextMenuAction(
        closeRequestId, QStringLiteral("close_tab:this")));
    QCOMPARE(cancellations.count(), 0);
    QVERIFY(!workspace.executeContextMenuAction(
        closeRequestId, QStringLiteral("close_tab:this")));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), tabCountBeforeClose - 1,
                              1000);
    QTRY_VERIFY_WITH_TIMEOUT(removedPane.isNull(), 1000);
    const QVector<TabId> remainingIds = tabIds(workspace);
    QVERIFY(!remainingIds.contains(first.tabId));
    QVERIFY(remainingIds.contains(second.tabId));
    QVERIFY(remainingIds.contains(created.tabId));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             created.tabId);
    QVERIFY(second.pane != nullptr);
}

void TerminalWorkspaceTest::contextMenuRoutesWindowAndConfigActions()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(900, 600);
    TerminalWorkspace workspace(window.contentItem());
    workspace.setSize(window.size());
    window.show();
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

    const CurrentTabProbe first = currentTabProbe(workspace);
    QVERIFY(first.pane != nullptr);
    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    const CurrentTabProbe second = currentTabProbe(workspace);
    QVERIFY(second.pane != nullptr);
    QVERIFY(second.tabId != first.tabId);
    QCOMPARE(window.activeFocusItem(), second.pane.data());

    QSignalSpy requests(&workspace, &TerminalWorkspace::contextMenuRequested);
    QSignalSpy cancellations(&workspace,
                             &TerminalWorkspace::contextMenuCancelled);
    QSignalSpy applicationActions(
        &workspace, &TerminalWorkspace::applicationActionRequested);
    QSignalSpy windowClose(&workspace, &TerminalWorkspace::windowCloseApproved);
    const auto requestForFirst = [&] {
        const int priorCount = requests.count();
        Q_EMIT first.pane->contextMenuRequested(QPointF(170.0, 120.0), false);
        if (requests.count() != priorCount + 1) return quint64{0};
        return requests.constLast().constFirst().toULongLong();
    };

    const quint64 firstRequestId = requestForFirst();
    QVERIFY(firstRequestId != 0);
    const std::array<QString, 10> rejectedActions{
        QStringLiteral("new_window:"),    QStringLiteral("new_window:now"),
        QStringLiteral("close_window:"),  QStringLiteral("close_window:now"),
        QStringLiteral("open_config:"),   QStringLiteral("open_config:now"),
        QStringLiteral("reload_config:"), QStringLiteral("reload_config:soft"),
        QStringLiteral("quit"),           QStringLiteral("close_all_windows"),
    };
    for (const QString &action : rejectedActions) {
        QVERIFY(!workspace.executeContextMenuAction(firstRequestId, action));
    }

    struct ApplicationCase {
        QString action;
        ApplicationAction expected;
    };
    const std::array<ApplicationCase, 3> applicationCases{{
        {QStringLiteral("new_window"), ApplicationAction::NewWindow},
        {QStringLiteral("open_config"), ApplicationAction::OpenConfig},
        {QStringLiteral("reload_config"), ApplicationAction::ReloadConfig},
    }};
    for (const ApplicationCase &testCase : applicationCases) {
        const quint64 requestId =
            applicationActions.isEmpty() ? firstRequestId : requestForFirst();
        const int priorActionCount = applicationActions.count();
        QVERIFY(requestId != 0);
        QVERIFY(workspace.executeContextMenuAction(requestId, testCase.action));
        QCOMPARE(applicationActions.count(), priorActionCount + 1);
        QCOMPARE(qvariant_cast<ApplicationAction>(
                     applicationActions.constLast().at(0)),
                 testCase.expected);
        QCOMPARE(qvariant_cast<PaneId>(applicationActions.constLast().at(1)),
                 first.paneId);
        QCOMPARE(window.activeFocusItem(), second.pane.data());
        QVERIFY(
            !workspace.executeContextMenuAction(requestId, testCase.action));
    }

    const quint64 closeRequestId = requestForFirst();
    QVERIFY(closeRequestId != 0);
    QVERIFY(workspace.executeContextMenuAction(closeRequestId,
                                               QStringLiteral("close_window")));
    QCOMPARE(windowClose.count(), 0);
    QCOMPARE(cancellations.count(), 0);
    QVERIFY(!workspace.executeContextMenuAction(
        closeRequestId, QStringLiteral("close_window")));
    QTRY_COMPARE_WITH_TIMEOUT(windowClose.count(), 1, 1000);
    QCOMPARE(applicationActions.count(), 3);
}

void TerminalWorkspaceTest::applicationQuitEscalatesCloseLifecycle()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    // A close_window,quit chain must retain the explicit application intent
    // after the ordinary close publication has completed.
    {
        TerminalWorkspace workspace;
        QSignalSpy windowClose(
            &workspace, &TerminalWorkspace::windowCloseApproved);
        QSignalSpy applicationQuit(
            &workspace, &TerminalWorkspace::applicationQuitApproved);
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        TerminalPane *const pane = workspace.findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);

        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("close_window")));
        QTRY_COMPARE_WITH_TIMEOUT(windowClose.count(), 1, 1000);
        QCOMPARE(applicationQuit.count(), 0);
        workspace.requestApplicationQuitConfirmation(
            workspace.closeAssessment());
        QCOMPARE(windowClose.count(), 1);
        QCOMPARE(applicationQuit.count(), 1);
        workspace.requestApplicationQuitConfirmation(
            workspace.closeAssessment());
        QCOMPARE(applicationQuit.count(), 1);
    }

    // Escalating an already pending window close reuses its one confirmation.
    {
        TerminalWorkspace workspace;
        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QSignalSpy windowClose(
            &workspace, &TerminalWorkspace::windowCloseApproved);
        QSignalSpy applicationQuit(
            &workspace, &TerminalWorkspace::applicationQuitApproved);
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        TerminalPane *const pane = workspace.findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("toggle_readonly")));

        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("close_window")));
        QTRY_COMPARE_WITH_TIMEOUT(confirmation.count(), 1, 1000);
        workspace.requestApplicationQuitConfirmation(
            workspace.closeAssessment());
        QCOMPARE(confirmation.count(), 1);
        workspace.confirmClose(closeConfirmationId(confirmation));
        QTRY_COMPARE_WITH_TIMEOUT(windowClose.count(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(applicationQuit.count(), 1, 1000);
    }

    // A process-wide quit supersedes a narrower dialog instead of being
    // swallowed by it. The replacement has a new ID and window-level copy.
    {
        TerminalWorkspace workspace;
        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QSignalSpy resolved(
            &workspace, &TerminalWorkspace::closeConfirmationResolved);
        QSignalSpy windowClose(
            &workspace, &TerminalWorkspace::windowCloseApproved);
        QSignalSpy applicationQuit(
            &workspace, &TerminalWorkspace::applicationQuitApproved);
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        TerminalPane *const pane = workspace.findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("toggle_readonly")));

        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("close_surface")));
        QCOMPARE(confirmation.count(), 1);
        const quint64 paneConfirmation = closeConfirmationId(confirmation);
        workspace.requestApplicationQuitConfirmation(
            workspace.closeAssessment());
        QCOMPARE(resolved.count(), 1);
        QCOMPARE(closeConfirmationId(resolved), paneConfirmation);
        QCOMPARE(confirmation.count(), 2);
        const quint64 windowConfirmation = closeConfirmationId(confirmation);
        QVERIFY(windowConfirmation != paneConfirmation);
        QCOMPARE(confirmation.constLast().at(1).toString(),
                 QStringLiteral(
                     "A terminal window contains a read-only pane. Quit the application?"));

        workspace.cancelClose(paneConfirmation);
        QCOMPARE(windowClose.count(), 0);
        workspace.confirmClose(windowConfirmation);
        QTRY_COMPARE_WITH_TIMEOUT(windowClose.count(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(applicationQuit.count(), 1, 1000);
    }

    // If another lifecycle event removes the final tab while the quit dialog
    // is pending, final-window convergence must preserve the application
    // intent rather than demoting it to an ordinary close.
    {
        TerminalWorkspace workspace;
        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QSignalSpy resolved(
            &workspace, &TerminalWorkspace::closeConfirmationResolved);
        QSignalSpy windowClose(
            &workspace, &TerminalWorkspace::windowCloseApproved);
        QSignalSpy applicationQuit(
            &workspace, &TerminalWorkspace::applicationQuitApproved);
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        TerminalPane *const pane = workspace.findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("toggle_readonly")));

        workspace.requestApplicationQuitConfirmation(
            workspace.closeAssessment());
        QCOMPARE(confirmation.count(), 1);
        {
            QSignalBlocker suppressPolicyReevaluation(pane);
            QVERIFY(pane->executeConfiguredAction(
                QStringLiteral("toggle_readonly")));
        }
        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("close_surface")));
        QCOMPARE(workspace.tabCount(), 0);
        QCOMPARE(resolved.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(windowClose.count(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(applicationQuit.count(), 1, 1000);
    }

    // Cancelling publishes resolution synchronously. A fresh quit requested
    // by that observer is newer than the cancelled intent and must not be
    // cleared when the original cancel call resumes.
    {
        TerminalWorkspace workspace;
        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QSignalSpy windowClose(
            &workspace, &TerminalWorkspace::windowCloseApproved);
        QSignalSpy applicationQuit(
            &workspace, &TerminalWorkspace::applicationQuitApproved);
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        TerminalPane *const pane = workspace.findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        QVERIFY(pane->executeConfiguredAction(
            QStringLiteral("toggle_readonly")));

        workspace.requestApplicationQuitConfirmation(
            workspace.closeAssessment());
        QCOMPARE(confirmation.count(), 1);
        const quint64 cancelledConfirmation =
            closeConfirmationId(confirmation);
        bool reentrantQuitAccepted = false;
        connect(&workspace, &TerminalWorkspace::closeConfirmationResolved,
                &workspace, [&](quint64 confirmationId) {
                    if (confirmationId == cancelledConfirmation) {
                        workspace.requestApplicationQuitConfirmation(
                            workspace.closeAssessment());
                        reentrantQuitAccepted = true;
                    }
                });

        workspace.cancelClose(cancelledConfirmation);
        QVERIFY(reentrantQuitAccepted);
        QCOMPARE(confirmation.count(), 2);
        const quint64 replacementConfirmation =
            closeConfirmationId(confirmation);
        QVERIFY(replacementConfirmation != cancelledConfirmation);
        workspace.confirmClose(replacementConfirmation);
        QTRY_COMPARE_WITH_TIMEOUT(windowClose.count(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(applicationQuit.count(), 1, 1000);
    }

    // Quit is lifecycle state, not a structural topology mutation. Requests
    // from a guarded close commit latch synchronously and reconcile once the
    // surviving topology is stable.
    {
        TerminalWorkspace workspace;
        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QSignalSpy windowClose(
            &workspace, &TerminalWorkspace::windowCloseApproved);
        QSignalSpy applicationQuit(
            &workspace, &TerminalWorkspace::applicationQuitApproved);
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        const CurrentTabProbe source = splitRightProbe(workspace);
        QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 2);
        QVERIFY(source.pane);
        QVERIFY(source.pane->executeConfiguredAction(
            QStringLiteral("toggle_readonly")));
        QVERIFY(source.pane->executeConfiguredAction(
            QStringLiteral("close_surface")));
        QCOMPARE(confirmation.count(), 1);
        const quint64 paneConfirmation = closeConfirmationId(confirmation);

        bool applicationActionAccepted = false;
        bool typedActionAccepted = false;
        connect(&workspace, &TerminalWorkspace::closeConfirmationResolved,
                &workspace, [&](quint64 confirmationId) {
                    if (confirmationId != paneConfirmation) return;
                    workspace.requestApplicationQuitConfirmation({});
                    applicationActionAccepted = true;
                    workspace.requestApplicationQuitConfirmation({});
                    typedActionAccepted = true;
                });

        workspace.confirmClose(paneConfirmation);
        QVERIFY(applicationActionAccepted);
        QVERIFY(typedActionAccepted);
        QCOMPARE(workspace.tabCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(applicationQuit.count(), 1, 1000);
        QCOMPARE(windowClose.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(source.pane.isNull(), 1000);
    }
}

void TerminalWorkspaceTest::closingOnlyPaneRemovesTab()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId paneId = workspace.tabModel()->entryAt(0)->activePaneId;

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ClosePane,
        {tabId, paneId, 0},
    }));
    QCOMPARE(workspace.tabCount(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
}

void TerminalWorkspaceTest::closeSurfaceUsesStableOriginsAndAdjacentFocus()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    // Splits and later tabs intentionally drop the frontend-only initial
    // program and hold. Keep their ordinary command alive so this topology
    // test cannot race Linux pidfd exit delivery.
    options.ordinaryCommand = TerminalCommand::direct({
        QByteArrayLiteral("/bin/sleep"),
        QByteArrayLiteral("30"),
    });
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(900, 600);
    TerminalWorkspace workspace;
    workspace.setParentItem(window.contentItem());
    workspace.setSize(window.size());
    window.show();
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const CurrentTabProbe first = currentTabProbe(workspace);
    const CurrentTabProbe second = splitRightProbe(workspace);
    const CurrentTabProbe third = splitRightProbe(workspace);
    const CurrentTabProbe fourth = splitRightProbe(workspace);
    QVERIFY(first.pane);
    QVERIFY(second.pane);
    QVERIFY(third.pane);
    QVERIFY(fourth.pane);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 4);
    QCOMPARE(splitDividerItems(&workspace).size(), 3);
    QCOMPARE(first.tabId, second.tabId);
    QCOMPARE(second.tabId, third.tabId);
    QCOMPARE(third.tabId, fourth.tabId);

    // Closing an inactive exact source must not steal focus from the tab's
    // active pane.
    QPointer<TerminalPane> secondPane(second.pane);
    QVERIFY(second.pane->executeConfiguredAction(
        QStringLiteral("close_surface")));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId,
             fourth.paneId);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(),
                              fourth.pane.data(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(secondPane.isNull(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(splitDividerItems(&workspace).size(), 2, 1000);

    bool nestedActionAttempted = false;
    bool nestedActionPerformed = true;
    const QMetaObject::Connection dataChanged = connect(
        workspace.tabModel(), &QAbstractItemModel::dataChanged,
        &workspace, [&] {
            if (nestedActionAttempted) return;
            nestedActionAttempted = true;
            nestedActionPerformed = workspace.dispatchAction({
                WorkspaceAction::NewTab,
                {first.tabId, third.paneId, 0},
            });
        });

    // D is the active rightmost leaf while A remains earlier in the tree.
    // Ghostty chooses adjacent C, not the first surviving leaf A.
    QPointer<TerminalPane> fourthPane(fourth.pane);
    QVERIFY(fourth.pane->executeConfiguredAction(
        QStringLiteral("close_surface")));
    QVERIFY(nestedActionAttempted);
    QVERIFY(!nestedActionPerformed);
    QCOMPARE(workspace.tabCount(), 1);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId,
             third.paneId);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(),
                              third.pane.data(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(fourthPane.isNull(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(splitDividerItems(&workspace).size(), 1, 1000);
    QObject::disconnect(dataChanged);

    // The leftmost leaf instead selects its next neighbor.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {first.tabId, first.paneId, 0},
    }));
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(),
                              first.pane.data(), 1000);
    QPointer<TerminalPane> firstPane(first.pane);
    QVERIFY(first.pane->executeConfiguredAction(
        QStringLiteral("close_surface")));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId,
             third.paneId);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(),
                              third.pane.data(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(firstPane.isNull(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(splitDividerItems(&workspace).size(), 0, 1000);
    QVERIFY(third.pane);
    QCOMPARE(third.pane->position(), workspace.boundingRect().topLeft());
    QCOMPARE(third.pane->size(), workspace.boundingRect().size());

    workspace.newTab();
    const CurrentTabProbe otherTab = currentTabProbe(workspace);
    QVERIFY(otherTab.pane);
    QCOMPARE(workspace.tabCount(), 2);

    // A context whose stable pane and tab identities disagree is stale and
    // must not close either target.
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ClosePane,
        {otherTab.tabId, third.paneId, 0},
    }));
    QCOMPARE(workspace.tabCount(), 2);
    QVERIFY(third.pane);

    // Closing the final pane of an inactive tab removes that source tab but
    // leaves the selected tab unchanged. Closing the final remaining pane
    // then requests shutdown of its containing window exactly once.
    QPointer<TerminalPane> thirdPane(third.pane);
    QVERIFY(third.pane->executeConfiguredAction(
        QStringLiteral("close_surface")));
    QCOMPARE(workspace.tabCount(), 1);
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             otherTab.tabId);
    QCOMPARE(window.activeFocusItem(), otherTab.pane.data());
    QCOMPARE(quit.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(thirdPane.isNull(), 1000);

    QPointer<TerminalPane> finalPane(otherTab.pane);
    QVERIFY(otherTab.pane->executeConfiguredAction(
        QStringLiteral("close_surface")));
    QCOMPARE(workspace.tabCount(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(finalPane.isNull(), 1000);
}

void TerminalWorkspaceTest::closeSurfaceConfirmationIsStableAndAtomic()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const CurrentTabProbe first = currentTabProbe(workspace);
    const CurrentTabProbe second = splitRightProbe(workspace);
    QVERIFY(first.pane);
    QVERIFY(second.pane);
    QVERIFY(first.pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QSignalSpy confirmation(
        &workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::closeConfirmationResolved);
    QVERIFY(first.pane->executeConfiguredAction(
        QStringLiteral("close_surface")));
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(confirmation.constFirst().at(1).toString(),
             QStringLiteral("This pane is read-only. Close it?"));
    const quint64 firstConfirmationId = closeConfirmationId(confirmation);
    QVERIFY(firstConfirmationId != 0);

    // Change focus and topology while the dialog is pending. Confirmation
    // remains attached to the original PaneId rather than the current pane.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {second.tabId, second.paneId, 0},
    }));
    const CurrentTabProbe third = splitRightProbe(workspace);
    QVERIFY(third.pane);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 3);

    bool resolvedObserverRan = false;
    bool nestedNewTabPerformed = true;
    bool nestedClosePerformed = true;
    const QMetaObject::Connection resolution = connect(
        &workspace, &TerminalWorkspace::closeConfirmationResolved,
        &workspace, [&](quint64 confirmationId) {
            if (confirmationId != firstConfirmationId) return;
            resolvedObserverRan = true;
            nestedNewTabPerformed = workspace.dispatchAction({
                WorkspaceAction::NewTab,
                {second.tabId, second.paneId, 0},
            });
            nestedClosePerformed = workspace.dispatchAction({
                WorkspaceAction::ClosePane,
                {first.tabId, first.paneId, 0},
            });
        });

    QPointer<TerminalPane> firstPane(first.pane);
    workspace.confirmClose(firstConfirmationId);
    QVERIFY(resolvedObserverRan);
    QVERIFY(!nestedNewTabPerformed);
    QVERIFY(!nestedClosePerformed);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(workspace.tabCount(), 1);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 3);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId,
             third.paneId);
    QTRY_VERIFY_WITH_TIMEOUT(firstPane.isNull(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        workspace.findChildren<TerminalPane *>().size(), 2, 1000);
    QObject::disconnect(resolution);

    QVERIFY(third.pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));
    QVERIFY(third.pane->executeConfiguredAction(
        QStringLiteral("close_surface")));
    QCOMPARE(confirmation.count(), 2);
    const quint64 secondConfirmationId = closeConfirmationId(confirmation);
    QVERIFY(secondConfirmationId != 0);
    QVERIFY(secondConfirmationId != firstConfirmationId);

    workspace.confirmClose(firstConfirmationId);
    workspace.cancelClose(firstConfirmationId);
    QCOMPARE(resolved.count(), 1);
    QVERIFY(third.pane);

    workspace.cancelClose(secondConfirmationId);
    QCOMPARE(resolved.count(), 2);
    QVERIFY(third.pane);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 2);
}

void TerminalWorkspaceTest::closeSurfaceConfirmsRunningProcess()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/close-surface-running-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString readyPath =
        directory.filePath(QStringLiteral("ready"));

    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.workingDirectory = directory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(": > \"$1\"; exec /bin/sleep 30"),
        QStringLiteral("close-surface-running"),
        readyPath,
    };
    options.confirmCloseMode = ConfirmCloseMode::RunningProcesses;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *const pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *const controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(readyPath), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->activeProcess(), 3000);

    QSignalSpy confirmation(
        &workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::closeConfirmationResolved);
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    QPointer<TerminalPane> guardedPane(pane);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("close_surface")));
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(confirmation.constFirst().at(1).toString(),
             QStringLiteral(
                 "A process is still running in this pane. Close it?"));
    QCOMPARE(workspace.tabCount(), 1);

    workspace.confirmClose(closeConfirmationId(confirmation));
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(workspace.tabCount(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(guardedPane.isNull(), 1000);
}

void TerminalWorkspaceTest::broadCloseSurfaceConvergesOnWorkspaceQuit()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    const auto exercise = [&](GhosttyKeybindFlags flags,
                              bool protectWithReadOnly,
                              Qt::Key key,
                              quint32 codepoint,
                              QChar controlCharacter) {
        LaunchOptions options = baseOptions();
        options.program = {QStringLiteral("/bin/true")};
        options.hold = true;
        options.confirmCloseMode = ConfirmCloseMode::Never;
        GhosttyKeybindConfig config;
        config.root = {GhosttyKeybindDefinition{
            .sequence = {GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::Unicode,
                .unicodeCodepoint = codepoint,
                .modifiers = GhosttyKeybindCtrl,
            }},
            .actions = {
                QStringLiteral("close_surface"),
                QStringLiteral("reload_config"),
            },
            .flags = flags,
        }};
        options.keybindSource =
            GhosttyKeybindSource::structured(std::move(config));
        TerminalWorkspace::setDefaultLaunchOptions(options);

        TerminalWorkspace workspace;
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        workspace.splitRight();
        workspace.newTab();
        const CurrentTabProbe source = currentTabProbe(workspace);
        QVERIFY(source.pane);
        QCOMPARE(workspace.tabCount(), 2);
        QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 3);
        if (protectWithReadOnly) {
            QVERIFY(source.pane->executeConfiguredAction(
                QStringLiteral("toggle_readonly")));
        }

        GhosttyApplicationKeybindings bindings(options, false);
        bindings.registerWorkspace(&workspace);
        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
        QSignalSpy reload(
            &bindings,
            &GhosttyApplicationKeybindings::applicationActionRequested);
        int confirmationsAtFirstReload = -1;
        int quitsAtFirstReload = -1;
        connect(&bindings,
                &GhosttyApplicationKeybindings::applicationActionRequested,
                &workspace, [&](ApplicationAction action) {
                    if (action != ApplicationAction::ReloadConfig) return;
                    if (confirmationsAtFirstReload >= 0) return;
                    confirmationsAtFirstReload = confirmation.count();
                    quitsAtFirstReload = quit.count();
                });
        TerminalController *const controller =
            source.pane->findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        QSignalSpy forwarded(controller, &TerminalController::keyRequested);

        bool quitObserverRan = false;
        bool nestedActionPerformed = true;
        connect(&workspace, &TerminalWorkspace::windowCloseApproved,
                &workspace, [&] {
                    quitObserverRan = true;
                    nestedActionPerformed = workspace.dispatchAction({
                        WorkspaceAction::NewTab,
                        {source.tabId, source.paneId, 0},
                    });
                });

        QKeyEvent press(QEvent::KeyPress, key, Qt::ControlModifier,
                        QString(controlCharacter));
        QCoreApplication::sendEvent(source.pane, &press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::ControlModifier);
        QCoreApplication::sendEvent(source.pane, &release);
        QCOMPARE(forwarded.count(), 0);
        QCOMPARE(reload.count(), 1);
        QCOMPARE(confirmationsAtFirstReload,
                 protectWithReadOnly ? 1 : 0);
        QCOMPARE(quitsAtFirstReload, 0);

        if (protectWithReadOnly) {
            QCOMPARE(confirmation.count(), 1);
            QCOMPARE(confirmation.constFirst().at(1).toString(),
                     QStringLiteral(
                         "This window contains a read-only pane. Quit?"));
            QCOMPARE(quit.count(), 0);
            workspace.confirmClose(closeConfirmationId(confirmation));
        } else {
            QCOMPARE(confirmation.count(), 0);
        }
        QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
        QVERIFY(quitObserverRan);
        QVERIFY(!nestedActionPerformed);
        QCOMPARE(workspace.tabCount(), 2);
        QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 3);
        QVERIFY(!workspace.dispatchAction({
            WorkspaceAction::NewTab,
            {source.tabId, source.paneId, 0},
        }));
        bindings.dispatchBroadActions({QStringLiteral("reload_config")});
        QCOMPARE(reload.count(), 2);
        QVERIFY(!workspace.executeSurfaceActionOnAllPanes(
            QStringLiteral("new_tab")));
        QCOMPARE(workspace.tabCount(), 2);
    };

    exercise(GhosttyKeybindFlags{.all = true},
             true,
             Qt::Key_S, 's', QChar(0x13));
    exercise(GhosttyKeybindFlags{.global = true},
             false,
             Qt::Key_G, 'g', QChar(0x07));

    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);
    TerminalWorkspace firstWorkspace;
    TerminalWorkspace secondWorkspace;
    QTRY_COMPARE_WITH_TIMEOUT(firstWorkspace.tabCount(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(secondWorkspace.tabCount(), 1, 1000);

    GhosttyApplicationKeybindings bindings(options, false);
    bindings.registerWorkspace(&firstWorkspace);
    bindings.registerWorkspace(&secondWorkspace);
    QSignalSpy firstQuit(&firstWorkspace,
                         &TerminalWorkspace::windowCloseApproved);
    QSignalSpy secondQuit(&secondWorkspace,
                          &TerminalWorkspace::windowCloseApproved);
    bindings.dispatchBroadActions({QStringLiteral("close_surface")});
    QTRY_COMPARE_WITH_TIMEOUT(firstQuit.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(secondQuit.count(), 1, 1000);
}

void TerminalWorkspaceTest::idleShellDoesNotPromptInRunningProcessesMode()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    QTest::qWait(350);

    workspace.requestWindowClose();
    QCOMPARE(confirmation.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
}

void TerminalWorkspaceTest::submittedCommandPromptsBeforeForegroundPoll()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    QTest::qWait(350);

    TerminalPane *pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return,
                    Qt::NoModifier, QStringLiteral("\r"));
    QCoreApplication::sendEvent(pane, &enter);

    // The UI-side latch closes the polling race: a close requested in the
    // same event turn as command submission must still protect active work.
    workspace.requestWindowClose();
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(quit.count(), 0);
    workspace.cancelClose(closeConfirmationId(confirmation));

    // An empty command settles back to an idle prompt after the conservative
    // grace interval and no longer needs confirmation.
    QTest::qWait(500);
    workspace.requestWindowClose();
    QCOMPARE(confirmation.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
}

void TerminalWorkspaceTest::terminalControlSubmissionPromptsBeforeWorkerRoundTrip()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    QTest::qWait(350);

    TerminalPane *pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    // Starting the worker and reaching the first idle prompt are independent
    // asynchronous transitions. Wait for both instead of assuming the
    // instrumented and non-instrumented builds settle within the same delay.
    QTRY_VERIFY_WITH_TIMEOUT(controller->running(), 1500);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->activeProcess(), 1500);

    // Malformed text remains consumed/no-op and must not manufacture active
    // work. A valid canonical newline, however, latches activity on the UI
    // side before its queued worker invocation can run.
    QVERIFY(pane->executeConfiguredAction(QStringLiteral(R"(text:\\q)")));
    QVERIFY(!controller->activeProcess());
    QVERIFY(pane->executeConfiguredAction(QStringLiteral(R"(text:\\n)")));
    QVERIFY(controller->activeProcess());

    workspace.requestWindowClose();
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(quit.count(), 0);
    workspace.cancelClose(closeConfirmationId(confirmation));
}

void TerminalWorkspaceTest::readOnlyBlocksUiActivityLatchAndProtectsClose()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

    TerminalPane *pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->activeProcess(), 1500);

    QSignalSpy readOnlyChanged(pane, &TerminalPane::readOnlyChanged);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));
    QVERIFY(pane->isReadOnly());
    QCOMPARE(readOnlyChanged.count(), 1);
    QVERIFY(workspace.tabModel()->entryAt(0)->readOnly);

    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return,
                    Qt::NoModifier, QStringLiteral("\r"));
    QCoreApplication::sendEvent(pane, &enter);
    QVERIFY(pane->executeConfiguredAction(QStringLiteral(R"(text:\\n)")));
    QVERIFY(!controller->activeProcess());

    // Read-only has close-policy precedence even though the shell is idle.
    workspace.requestWindowClose();
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(quit.count(), 0);
    workspace.cancelClose(closeConfirmationId(confirmation));

    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));
    QVERIFY(!pane->isReadOnly());
    QCOMPARE(readOnlyChanged.count(), 2);
    QVERIFY(!workspace.tabModel()->entryAt(0)->readOnly);

    // The same action immediately regains the normal UI-side activity latch.
    QVERIFY(pane->executeConfiguredAction(QStringLiteral(R"(text:\\n)")));
    QVERIFY(controller->activeProcess());
}

void TerminalWorkspaceTest::readOnlyStateIsPaneLocalAndBroadFanoutIsStable()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQmlEngine engine;
    const QString overlayPath =
        QFINDTESTDATA("../qml/ReadOnlyOverlay.qml");
    QVERIFY(!overlayPath.isEmpty());
    QQmlComponent overlayComponent(
        &engine, QUrl::fromLocalFile(overlayPath));
    QVERIFY2(overlayComponent.isReady(),
             qPrintable(overlayComponent.errorString()));

    QQuickWindow window;
    window.resize(900, 600);
    window.show();
    TerminalWorkspace workspace;
    workspace.setParentItem(window.contentItem());
    workspace.setSize(window.size());
    workspace.setReadOnlyOverlayComponent(&overlayComponent);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId firstId = workspace.tabModel()->entryAt(0)->activePaneId;

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    QTRY_COMPARE_WITH_TIMEOUT(
        workspace.findChildren<TerminalPane *>().size(), 2, 1000);
    const PaneId secondId = workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(secondId != firstId);
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane != firstPane) secondPane = pane;
    }
    QVERIFY(secondPane != nullptr);

    workspace.setReadOnlyOverlayComponent(nullptr);
    QVERIFY(firstPane->findChild<QQuickItem *>(
                QStringLiteral("terminalReadOnlyOverlay"),
                Qt::FindDirectChildrenOnly)
            == nullptr);
    QVERIFY(secondPane->findChild<QQuickItem *>(
                QStringLiteral("terminalReadOnlyOverlay"),
                Qt::FindDirectChildrenOnly)
            == nullptr);
    workspace.setReadOnlyOverlayComponent(&overlayComponent);

    auto *firstOverlay = firstPane->findChild<QQuickItem *>(
        QStringLiteral("terminalReadOnlyOverlay"),
        Qt::FindDirectChildrenOnly);
    auto *secondOverlay = secondPane->findChild<QQuickItem *>(
        QStringLiteral("terminalReadOnlyOverlay"),
        Qt::FindDirectChildrenOnly);
    QVERIFY(firstOverlay != nullptr);
    QVERIFY(secondOverlay != nullptr);
    QVERIFY(!firstOverlay->isEnabled());
    QVERIFY(!secondOverlay->isEnabled());
    QCOMPARE(firstOverlay->parentItem(), firstPane);
    QCOMPARE(secondOverlay->parentItem(), secondPane);

    QSignalSpy firstChanged(firstPane, &TerminalPane::readOnlyChanged);
    QSignalSpy secondChanged(secondPane, &TerminalPane::readOnlyChanged);
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_readonly")));
    QVERIFY(firstPane->isReadOnly());
    QVERIFY(secondPane->isReadOnly());
    QCOMPARE(firstChanged.count(), 1);
    QCOMPARE(secondChanged.count(), 1);
    QVERIFY(workspace.tabModel()->entryAt(0)->readOnly);
    QTRY_VERIFY_WITH_TIMEOUT(firstOverlay->isVisible(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(secondOverlay->isVisible(), 1000);
    QCOMPARE(firstOverlay->y(), 8.0);
    QCOMPARE(secondOverlay->y(), 8.0);
    QCOMPARE(firstOverlay->x(),
             std::max(0.0, firstPane->width() - firstOverlay->width() - 8.0));
    QCOMPARE(secondOverlay->x(),
             std::max(0.0, secondPane->width() - secondOverlay->width() - 8.0));

    // Read-only filters PTY-directed input, not terminal-local work.
    TerminalController *firstController =
        firstPane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QSignalSpy viewport(firstController,
                        &TerminalController::scrollRequested);
    QSignalSpy reset(firstController,
                     &TerminalController::resetTerminalRequested);
    QSignalSpy search(firstController,
                      &TerminalController::serializedSearchRequested);
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("scroll_to_top")));
    QVERIFY(firstPane->executeConfiguredAction(QStringLiteral("reset")));
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("search:still-local")));
    QCOMPARE(viewport.count(), 1);
    QCOMPARE(reset.count(), 1);
    QCOMPARE(search.count(), 1);

    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_readonly")));
    QVERIFY(!firstPane->isReadOnly());
    QVERIFY(!secondPane->isReadOnly());
    QCOMPARE(firstChanged.count(), 2);
    QCOMPARE(secondChanged.count(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!firstOverlay->isVisible(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!secondOverlay->isVisible(), 1000);

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));
    QVERIFY(firstPane->isReadOnly());
    QVERIFY(!secondPane->isReadOnly());

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {tabId, firstId, 0},
    }));
    QVERIFY(workspace.tabModel()->entryAt(0)->readOnly);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {tabId, secondId, 0},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->readOnly);

    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy resolved(&workspace,
                        &TerminalWorkspace::closeConfirmationResolved);
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);

    // An inactive read-only pane protects its containing tab and window.
    workspace.closeCurrentTab();
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(workspace.tabCount(), 1);
    workspace.cancelClose(closeConfirmationId(confirmation));
    workspace.requestWindowClose();
    QCOMPARE(confirmation.count(), 2);
    QCOMPARE(quit.count(), 0);
    workspace.cancelClose(closeConfirmationId(confirmation));

    // It also protects its own pane. Removing read-only while that request is
    // pending re-evaluates and completes the now-safe close exactly once.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ClosePane,
        {tabId, firstId, 0},
    }));
    QCOMPARE(confirmation.count(), 3);
    QPointer<TerminalPane> removedPane(firstPane);
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));
    QTRY_VERIFY_WITH_TIMEOUT(removedPane.isNull(), 1000);
    // Both explicit cancellations and the auto-completed pane close resolve
    // their distinct confirmation identities.
    QCOMPARE(resolved.count(), 3);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 1);

    workspace.requestWindowClose();
    QCOMPARE(confirmation.count(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
}

void TerminalWorkspaceTest::abnormalExitOverlayPresentsHeldFailureAndCloses()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {
        QStringLiteral("/ghostty-qt-test/nonexistent-child"),
    };
    options.hold = true;
    options.abnormalCommandExitRuntimeMilliseconds =
        std::numeric_limits<quint32>::max();
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQmlEngine engine;
    const QString overlayPath = QFINDTESTDATA("../qml/AbnormalExitOverlay.qml");
    QVERIFY(!overlayPath.isEmpty());
    QQmlComponent overlayComponent(&engine, QUrl::fromLocalFile(overlayPath));
    QVERIFY2(overlayComponent.isReady(),
             qPrintable(overlayComponent.errorString()));

    QQuickWindow window;
    window.resize(900, 600);
    window.show();
    TerminalWorkspace workspace;
    workspace.setParentItem(window.contentItem());
    workspace.setSize(window.size());
    workspace.setAbnormalExitOverlayComponent(&overlayComponent);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

    TerminalPane *const pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *const controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(controller->sessionStarted(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->running(), 1000);

    QQuickItem *const overlay = pane->findChild<QQuickItem *>(
        QStringLiteral("terminalAbnormalExitOverlay"),
        Qt::FindDirectChildrenOnly);
    QVERIFY(overlay != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(overlay->isVisible(), 1000);
    QCOMPARE(overlay->parentItem(), pane);
    QCOMPARE(overlay->x(), 0.0);
    QCOMPARE(overlay->width(), pane->width());
    QCOMPARE(overlay->y() + overlay->height(), pane->height());

    QQuickItem *const message = overlay->findChild<QQuickItem *>(
        QStringLiteral("terminalAbnormalExitMessage"));
    QVERIFY(message != nullptr);
    const QString messageText = message->property("text").toString();
    QVERIFY(messageText.startsWith(QStringLiteral("Command failed after ")));
    QVERIFY(messageText.endsWith(QStringLiteral(" ms (exit status 127).")));

    QQuickItem *const closeButton = overlay->findChild<QQuickItem *>(
        QStringLiteral("terminalAbnormalExitCloseButton"));
    QVERIFY(closeButton != nullptr);
    QCOMPARE(closeButton->focusPolicy(), Qt::TabFocus);
    closeButton->forceActiveFocus(Qt::TabFocusReason);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), closeButton, 1000);

    QPointer<TerminalPane> closedPane(pane);
    QVERIFY(
        QMetaObject::invokeMethod(closeButton, "click", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(closedPane.isNull(), 1000);
    QCOMPARE(workspace.tabCount(), 0);
}

void TerminalWorkspaceTest::overlayComponentsShareOneLifecycle()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQmlEngine engine;
    using Setter = void (TerminalWorkspace::*)(QQmlComponent *);
    using Getter = QQmlComponent *(TerminalWorkspace::*)() const;
    using ChangedSignal = void (TerminalWorkspace::*)();
    struct OverlayCase {
        const char *name;
        Setter setter;
        Getter getter;
        ChangedSignal changed;
    };
    const std::array cases{
        OverlayCase{
            "search",
            &TerminalWorkspace::setSearchOverlayComponent,
            &TerminalWorkspace::searchOverlayComponent,
            &TerminalWorkspace::searchOverlayComponentChanged,
        },
        OverlayCase{
            "abnormalExit",
            &TerminalWorkspace::setAbnormalExitOverlayComponent,
            &TerminalWorkspace::abnormalExitOverlayComponent,
            &TerminalWorkspace::abnormalExitOverlayComponentChanged,
        },
        OverlayCase{
            "readOnly",
            &TerminalWorkspace::setReadOnlyOverlayComponent,
            &TerminalWorkspace::readOnlyOverlayComponent,
            &TerminalWorkspace::readOnlyOverlayComponentChanged,
        },
        OverlayCase{
            "resize",
            &TerminalWorkspace::setResizeOverlayComponent,
            &TerminalWorkspace::resizeOverlayComponent,
            &TerminalWorkspace::resizeOverlayComponentChanged,
        },
        OverlayCase{
            "scrollbar",
            &TerminalWorkspace::setScrollbarComponent,
            &TerminalWorkspace::scrollbarComponent,
            &TerminalWorkspace::scrollbarComponentChanged,
        },
        OverlayCase{
            "bellBorder",
            &TerminalWorkspace::setBellBorderComponent,
            &TerminalWorkspace::bellBorderComponent,
            &TerminalWorkspace::bellBorderComponentChanged,
        },
    };

    const auto makeComponent = [&engine](const QString &objectName) {
        auto component = std::make_unique<QQmlComponent>(&engine);
        const QByteArray source = QStringLiteral(
            "import QtQuick\n"
            "import GhosttyQt 1.0\n"
            "Item {\n"
            "    required property TerminalPane terminalPane\n"
            "    objectName: \"%1\"\n"
            "    enabled: false\n"
            "}\n")
                                      .arg(objectName)
                                      .toUtf8();
        component->setData(
            source,
            QUrl(QStringLiteral("inmemory:/%1.qml").arg(objectName)));
        return component;
    };

    for (const OverlayCase &overlayCase : cases) {
        TerminalWorkspace workspace;
        workspace.setSize(QSizeF(640.0, 360.0));
        QVERIFY(workspace.initialize(options));
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        QSignalSpy changed(&workspace, overlayCase.changed);

        const QString firstName = QStringLiteral("%1OverlayFirst")
                                      .arg(QString::fromLatin1(
                                          overlayCase.name));
        auto firstComponent = makeComponent(firstName);
        QTRY_VERIFY_WITH_TIMEOUT(
            firstComponent->isReady() || firstComponent->isError(), 1000);
        QVERIFY2(firstComponent->isReady(),
                 qPrintable(firstComponent->errorString()));
        (workspace.*overlayCase.setter)(firstComponent.get());
        QCOMPARE((workspace.*overlayCase.getter)(), firstComponent.get());
        QCOMPARE(changed.count(), 1);

        TerminalPane *const initialPane =
            workspace.findChild<TerminalPane *>();
        QVERIFY(initialPane != nullptr);
        QPointer<QQuickItem> firstOverlay =
            initialPane->findChild<QQuickItem *>(
                firstName, Qt::FindDirectChildrenOnly);
        QVERIFY(firstOverlay != nullptr);
        QCOMPARE(firstOverlay->parentItem(), initialPane);
        QCOMPARE(firstOverlay->property("terminalPane").value<TerminalPane *>(),
                 initialPane);

        // Reassigning the same externally owned factory is a no-op and must
        // retain the existing pane object rather than causing QML churn.
        (workspace.*overlayCase.setter)(firstComponent.get());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(initialPane->findChild<QQuickItem *>(
                     firstName, Qt::FindDirectChildrenOnly),
                 firstOverlay.data());

        const QString secondName = QStringLiteral("%1OverlaySecond")
                                       .arg(QString::fromLatin1(
                                           overlayCase.name));
        auto secondComponent = makeComponent(secondName);
        QTRY_VERIFY_WITH_TIMEOUT(
            secondComponent->isReady() || secondComponent->isError(), 1000);
        QVERIFY2(secondComponent->isReady(),
                 qPrintable(secondComponent->errorString()));
        (workspace.*overlayCase.setter)(secondComponent.get());
        QCOMPARE((workspace.*overlayCase.getter)(), secondComponent.get());
        QCOMPARE(changed.count(), 2);
        QVERIFY(firstOverlay.isNull());

        QPointer<QQuickItem> initialReplacement =
            initialPane->findChild<QQuickItem *>(
                secondName, Qt::FindDirectChildrenOnly);
        QVERIFY(initialReplacement != nullptr);
        workspace.splitRight();
        QTRY_COMPARE_WITH_TIMEOUT(
            workspace.findChildren<TerminalPane *>().size(), 2, 1000);
        for (TerminalPane *const pane :
             workspace.findChildren<TerminalPane *>()) {
            QQuickItem *const attached = pane->findChild<QQuickItem *>(
                secondName, Qt::FindDirectChildrenOnly);
            QVERIFY(attached != nullptr);
            QCOMPARE(attached->parentItem(), pane);
            QCOMPARE(attached->property("terminalPane").value<TerminalPane *>(),
                     pane);
        }

        // Destroying the externally owned current factory clears its guarded
        // pointer and every product. This was the stale-search-overlay bug.
        secondComponent.reset();
        QCOMPARE((workspace.*overlayCase.getter)(), nullptr);
        QCOMPARE(changed.count(), 3);
        QVERIFY(initialReplacement.isNull());
        for (TerminalPane *const pane :
             workspace.findChildren<TerminalPane *>()) {
            QVERIFY(pane->findChild<QQuickItem *>(
                        secondName, Qt::FindDirectChildrenOnly)
                    == nullptr);
        }

        (workspace.*overlayCase.setter)(nullptr);
        QCOMPARE(changed.count(), 3);

        const QString thirdName = QStringLiteral("%1OverlayThird")
                                      .arg(QString::fromLatin1(
                                          overlayCase.name));
        auto thirdComponent = makeComponent(thirdName);
        QTRY_VERIFY_WITH_TIMEOUT(
            thirdComponent->isReady() || thirdComponent->isError(), 1000);
        QVERIFY2(thirdComponent->isReady(),
                 qPrintable(thirdComponent->errorString()));
        (workspace.*overlayCase.setter)(thirdComponent.get());
        QCOMPARE(changed.count(), 4);
        (workspace.*overlayCase.setter)(nullptr);
        QCOMPARE(changed.count(), 5);
        QCOMPARE((workspace.*overlayCase.getter)(), nullptr);
        for (TerminalPane *const pane :
             workspace.findChildren<TerminalPane *>()) {
            QVERIFY(pane->findChild<QQuickItem *>(
                        thirdName, Qt::FindDirectChildrenOnly)
                    == nullptr);
        }
    }
}

void TerminalWorkspaceTest::pendingPaneReloadsDuringOverlayCompletion()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions initial = baseOptions();
    initial.program = {QStringLiteral("/bin/true")};
    initial.hold = true;
    initial.typography.pointSize = 10.0;
    initial.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+a=ignore"),
    });
    TerminalWorkspace::setDefaultLaunchOptions(initial);

    QQmlEngine engine;
    QQmlComponent overlay(&engine);
    overlay.setData(
        QByteArrayLiteral(
            "import QtQuick\n"
            "import GhosttyQt 1.0\n"
            "Item {\n"
            "    required property TerminalPane terminalPane\n"
            "    Component.onCompleted: {\n"
            "        if (terminalPane.objectName.length === 0)\n"
            "            terminalPane.objectName = 'reload-during-overlay'\n"
            "    }\n"
            "}\n"),
        QUrl(QStringLiteral("inmemory:/ReloadingOverlay.qml")));
    QTRY_VERIFY_WITH_TIMEOUT(overlay.isReady() || overlay.isError(), 1000);
    QVERIFY2(overlay.isReady(), qPrintable(overlay.errorString()));

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(640.0, 360.0));
    QVERIFY(workspace.initialize(initial));
    TerminalPane *const initialPane =
        workspace.findChild<TerminalPane *>();
    QVERIFY(initialPane != nullptr);
    initialPane->setObjectName(QStringLiteral("existing-pane"));
    workspace.setSearchOverlayComponent(&overlay);

    LaunchOptions pendingOptions = initial;
    pendingOptions.typography.pointSize = 18.0;
    pendingOptions.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+b=ignore"),
    });
    GhosttyKeybindProgram pendingProgram =
        GhosttyKeybindProgram::compile(
            pendingOptions.keybindSource).program;
    int reloadCount = 0;

    connect(&workspace, &QQuickItem::childrenChanged,
            &workspace, [&] {
                for (TerminalPane *pane :
                     workspace.findChildren<TerminalPane *>()) {
                    if (pane->property("_ghosttyQtReloadHook").toBool()) {
                        continue;
                    }
                    pane->setProperty("_ghosttyQtReloadHook", true);
                    connect(pane, &QObject::objectNameChanged,
                            &workspace,
                            [&, paneGuard = QPointer(pane)](
                                const QString &name) {
                                if (paneGuard == nullptr
                                    || name
                                        != QStringLiteral(
                                            "reload-during-overlay")) {
                                    return;
                                }
                                ++reloadCount;
                                workspace.applyLaunchOptions(
                                    pendingOptions, pendingProgram);
                            });
                }
            });

    workspace.newTab();
    QCOMPARE(reloadCount, 1);
    QCOMPARE(workspace.tabCount(), 2);
    QVERIFY(workspace.keybindProgram().isSameGeneration(pendingProgram));
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        QCOMPARE(pane->fontPointSize(),
                 pendingOptions.typography.pointSize);
        QVERIFY(pane->keybindProgram().isSameGeneration(pendingProgram));
        pane->setObjectName(QStringLiteral("existing-pane"));
    }

    pendingOptions.typography.pointSize = 22.0;
    pendingOptions.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+c=ignore"),
    });
    pendingProgram = GhosttyKeybindProgram::compile(
        pendingOptions.keybindSource).program;
    workspace.splitRight();

    QCOMPARE(reloadCount, 2);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 3);
    QVERIFY(workspace.keybindProgram().isSameGeneration(pendingProgram));
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        QCOMPARE(pane->fontPointSize(),
                 pendingOptions.typography.pointSize);
        QVERIFY(pane->keybindProgram().isSameGeneration(pendingProgram));
    }
}

void TerminalWorkspaceTest::resizeOverlayIsPaneLocalAndScalesWithDpr()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.resizeOverlay = {
        .mode = ResizeOverlayMode::AfterFirst,
        .position = ResizeOverlayPosition::TopCenter,
        .duration = std::chrono::milliseconds(250),
    };
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQmlEngine engine;
    const QString overlayPath =
        QFINDTESTDATA("../qml/ResizeOverlay.qml");
    QVERIFY(!overlayPath.isEmpty());
    QQmlComponent overlayComponent(
        &engine, QUrl::fromLocalFile(overlayPath));
    QVERIFY2(overlayComponent.isReady(),
             qPrintable(overlayComponent.errorString()));

    QQuickWindow window;
    window.resize(902, 602);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    workspace->setResizeOverlayComponent(&overlayComponent);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
    if (qEnvironmentVariableIsSet("GHOSTTY_QT_EXPECT_HIDPI")) {
        QVERIFY(window.devicePixelRatio() >= 2.0);
    }
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);

    const TabId tabId = workspace->tabModel()->idAt(0);
    const PaneId firstId =
        workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace->findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    QTRY_COMPARE_WITH_TIMEOUT(
        workspace->findChildren<TerminalPane *>().size(), 2, 1000);
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane) secondPane = pane;
    }
    QVERIFY(secondPane != nullptr);

    auto *firstOverlay = firstPane->findChild<QQuickItem *>(
        QStringLiteral("terminalResizeOverlay"),
        Qt::FindDirectChildrenOnly);
    auto *secondOverlay = secondPane->findChild<QQuickItem *>(
        QStringLiteral("terminalResizeOverlay"),
        Qt::FindDirectChildrenOnly);
    QVERIFY(firstOverlay != nullptr);
    QVERIFY(secondOverlay != nullptr);
    QVERIFY(!firstOverlay->isEnabled());
    QVERIFY(!secondOverlay->isEnabled());
    QCOMPARE(firstOverlay->parentItem(), firstPane);
    QCOMPARE(secondOverlay->parentItem(), secondPane);
    QCOMPARE(firstOverlay->size(), QSizeF(120.0, 40.0));
    QCOMPARE(secondOverlay->size(), QSizeF(120.0, 40.0));
    QTRY_VERIFY_WITH_TIMEOUT(
        !firstPane->resizeOverlayVisible()
            && !secondPane->resizeOverlayVisible(),
        1000);

    LaunchOptions visibleOptions = options;
    visibleOptions.resizeOverlay.duration = std::chrono::milliseconds(2'000);
    workspace->applyLaunchOptions(visibleOptions);
    QTest::qWait(275);
    firstPane->setWidth(firstPane->width() + 100.0);
    QTRY_VERIFY_WITH_TIMEOUT(firstPane->resizeOverlayVisible(), 1000);
    QVERIFY(!secondPane->resizeOverlayVisible());
    QTRY_VERIFY_WITH_TIMEOUT(firstOverlay->isVisible(), 1000);
    QVERIFY(!secondOverlay->isVisible());
    QCOMPARE(firstOverlay->position(),
             firstPane->resizeOverlayRect().topLeft());
    QCOMPARE(firstOverlay->size(), firstPane->resizeOverlayRect().size());

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    const qreal physicalScale =
        static_cast<qreal>(frame.width()) / window.width();
    QVERIFY(qAbs(physicalScale - window.devicePixelRatio()) < 0.01);
    QCOMPARE(qRound(firstOverlay->width() * physicalScale),
             qRound(120.0 * window.devicePixelRatio()));
    QCOMPARE(qRound(firstOverlay->height() * physicalScale),
             qRound(40.0 * window.devicePixelRatio()));

    LaunchOptions reloaded = visibleOptions;
    reloaded.resizeOverlay.position = ResizeOverlayPosition::BottomRight;
    reloaded.resizeOverlay.duration = std::chrono::milliseconds(500);
    workspace->applyLaunchOptions(reloaded);
    QCOMPARE(firstOverlay->position(),
             firstPane->resizeOverlayRect().topLeft());
    QCOMPARE(firstPane->resizeOverlayRect().bottomRight(),
             QPointF(firstPane->width(), firstPane->height()));
    QVERIFY(firstPane->resizeOverlayVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!firstPane->resizeOverlayVisible(), 750);

    firstPane->setWidth(firstPane->width() + 100.0);
    QTRY_VERIFY_WITH_TIMEOUT(firstPane->resizeOverlayVisible(), 1000);

    reloaded.resizeOverlay.mode = ResizeOverlayMode::Never;
    workspace->applyLaunchOptions(reloaded);
    QVERIFY(!firstPane->resizeOverlayVisible());
    QVERIFY(!secondPane->resizeOverlayVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!firstOverlay->isVisible(), 1000);

    workspace->setResizeOverlayComponent(nullptr);
    QVERIFY(firstPane->findChild<QQuickItem *>(
                QStringLiteral("terminalResizeOverlay"),
                Qt::FindDirectChildrenOnly)
            == nullptr);
    QVERIFY(secondPane->findChild<QQuickItem *>(
                QStringLiteral("terminalResizeOverlay"),
                Qt::FindDirectChildrenOnly)
            == nullptr);
    workspace.reset();
    window.close();
}

void TerminalWorkspaceTest::scrollbarOverlayIsPaneLocalAndReloadable()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.scrollbar = ScrollbarPolicy::System;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQmlEngine engine;
    const QString overlayPath = QFINDTESTDATA("../qml/TerminalScrollBar.qml");
    QVERIFY(!overlayPath.isEmpty());
    QQmlComponent overlayComponent(&engine, QUrl::fromLocalFile(overlayPath));
    QVERIFY2(overlayComponent.isReady(),
             qPrintable(overlayComponent.errorString()));

    QQuickWindow window;
    window.resize(900, 600);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    workspace->setScrollbarComponent(&overlayComponent);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);

    TerminalPane *const firstPane = workspace->findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    auto *const firstScrollbar = firstPane->findChild<QQuickItem *>(
        QStringLiteral("terminalScrollBar"), Qt::FindDirectChildrenOnly);
    QVERIFY(firstScrollbar != nullptr);
    QCOMPARE(firstScrollbar->parentItem(), firstPane);
    QVERIFY(!firstScrollbar->isVisible());

    quint64 revision = 1;
    const auto publishScrollbar = [&revision](TerminalPane *pane, quint64 total,
                                              quint64 offset, quint64 length) {
        TerminalController *const controller =
            pane->findChild<TerminalController *>();
        if (controller == nullptr) return false;

        TerminalUpdate update;
        update.columns = 2;
        update.rows = 2;
        update.fullFrame = true;
        update.scrollTotal = total;
        update.scrollOffset = offset;
        update.scrollLength = length;
        update.contentRevision = revision++;
        for (int row = 0; row < update.rows; ++row) {
            TerminalRowUpdate rowUpdate;
            rowUpdate.row = row;
            rowUpdate.cells.resize(update.columns);
            update.dirtyRows.append(std::move(rowUpdate));
        }
        controller->terminalUpdated(update);
        return true;
    };

    QVERIFY(publishScrollbar(firstPane, 100, 20, 25));
    QTRY_VERIFY_WITH_TIMEOUT(firstScrollbar->isVisible(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(firstScrollbar->property("size").toReal(), 0.25,
                              1000);
    QTRY_COMPARE_WITH_TIMEOUT(firstScrollbar->property("position").toReal(),
                              0.2, 1000);

    TerminalController *const firstController =
        firstPane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QSignalSpy viewportRequests(firstController,
                                &TerminalController::scrollRequested);

    // Exercise the real Qt control rather than only its C++ projection. A
    // track click and a thumb drag must both enter the existing absolute-row
    // viewport path.
    const QPoint trackPoint =
        firstScrollbar
            ->mapToScene(QPointF(firstScrollbar->width() / 2.0,
                                 firstScrollbar->height() * 0.85))
            .toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, trackPoint);
    QTRY_VERIFY_WITH_TIMEOUT(!viewportRequests.isEmpty(), 1000);
    const auto requestAt = [&viewportRequests](int index) {
        return qvariant_cast<TerminalViewportRequest>(
            viewportRequests.at(index).constFirst());
    };
    QCOMPARE(requestAt(0).kind, TerminalViewportRequest::Kind::Row);
    QVERIFY(requestAt(0).row > quint64{20});
    QVERIFY(requestAt(0).row <= quint64{75});

    QVERIFY(publishScrollbar(firstPane, 100, requestAt(0).row, 25));
    const int clickRequestCount = viewportRequests.count();
    QTRY_COMPARE_WITH_TIMEOUT(firstScrollbar->property("position").toReal(),
                              static_cast<qreal>(requestAt(0).row) / 100.0,
                              1000);
    QCOMPARE(viewportRequests.count(), clickRequestCount);

    // Qt owns the platform-specific pointer gesture. Drive the same position
    // transition deterministically to verify that the QML binding forwards
    // non-pointer/accessibility movement while rejecting passive worker
    // updates.
    const qreal dragPosition =
        firstScrollbar->property("position").toReal() > 0.35 ? 0.1 : 0.65;
    QVERIFY(firstScrollbar->setProperty("position", dragPosition));
    QTRY_VERIFY_WITH_TIMEOUT(viewportRequests.count() > clickRequestCount,
                             1000);
    QCOMPARE(requestAt(viewportRequests.count() - 1).kind,
             TerminalViewportRequest::Kind::Row);

    QSignalSpy runtimeOptions(firstController,
                              &TerminalController::runtimeOptionsRequested);

    const QRectF originalGeometry(firstPane->position(), firstPane->size());
    LaunchOptions hidden = options;
    hidden.scrollbar = ScrollbarPolicy::Never;
    workspace->applyLaunchOptions(hidden);
    QCOMPARE(workspace->findChild<TerminalPane *>(), firstPane);
    QCOMPARE(QRectF(firstPane->position(), firstPane->size()),
             originalGeometry);
    QTRY_VERIFY_WITH_TIMEOUT(!firstScrollbar->isVisible(), 1000);
    QCOMPARE(runtimeOptions.count(), 0);

    workspace->applyLaunchOptions(options);
    QCOMPARE(workspace->findChild<TerminalPane *>(), firstPane);
    QCOMPARE(QRectF(firstPane->position(), firstPane->size()),
             originalGeometry);
    QTRY_VERIFY_WITH_TIMEOUT(firstScrollbar->isVisible(), 1000);
    QCOMPARE(runtimeOptions.count(), 0);

    const TabId tabId = workspace->tabModel()->idAt(0);
    const PaneId firstId = workspace->tabModel()->entryAt(0)->activePaneId;
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    QTRY_COMPARE_WITH_TIMEOUT(workspace->findChildren<TerminalPane *>().size(),
                              2, 1000);
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane) secondPane = pane;
    }
    QVERIFY(secondPane != nullptr);
    auto *const secondScrollbar = secondPane->findChild<QQuickItem *>(
        QStringLiteral("terminalScrollBar"), Qt::FindDirectChildrenOnly);
    QVERIFY(secondScrollbar != nullptr);
    QVERIFY(secondScrollbar != firstScrollbar);
    QCOMPARE(secondScrollbar->parentItem(), secondPane);
    QVERIFY(publishScrollbar(secondPane, 200, 50, 40));
    QTRY_VERIFY_WITH_TIMEOUT(secondScrollbar->isVisible(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(secondScrollbar->property("size").toReal(), 0.2,
                              1000);
    QTRY_COMPARE_WITH_TIMEOUT(secondScrollbar->property("position").toReal(),
                              0.25, 1000);

    const PaneId secondId = workspace->tabModel()->entryAt(0)->activePaneId;
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QCOMPARE(
        secondPane->findChild<QQuickItem *>(QStringLiteral("terminalScrollBar"),
                                            Qt::FindDirectChildrenOnly),
        secondScrollbar);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QCOMPARE(
        firstPane->findChild<QQuickItem *>(QStringLiteral("terminalScrollBar"),
                                           Qt::FindDirectChildrenOnly),
        firstScrollbar);
    QCOMPARE(
        secondPane->findChild<QQuickItem *>(QStringLiteral("terminalScrollBar"),
                                            Qt::FindDirectChildrenOnly),
        secondScrollbar);

    // A viewport without history (including the alternate screen's ordinary
    // metadata shape) hides only its own control.
    QVERIFY(publishScrollbar(secondPane, 40, 0, 40));
    QTRY_VERIFY_WITH_TIMEOUT(!secondScrollbar->isVisible(), 1000);
    QVERIFY(firstScrollbar->isVisible());

    workspace->setScrollbarComponent(nullptr);
    QVERIFY(firstPane->findChild<QQuickItem *>(
                QStringLiteral("terminalScrollBar"), Qt::FindDirectChildrenOnly)
            == nullptr);
    QVERIFY(secondPane->findChild<QQuickItem *>(
                QStringLiteral("terminalScrollBar"), Qt::FindDirectChildrenOnly)
            == nullptr);
    workspace.reset();
    window.close();
}

void TerminalWorkspaceTest::bellBorderOverlayIsPaneLocalAndInputTransparent()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.bellFeatures.attention = false;
    options.bellFeatures.title = false;
    options.bellFeatures.border = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQmlEngine engine;
    const QString overlayPath = QFINDTESTDATA("../qml/BellBorderOverlay.qml");
    QVERIFY(!overlayPath.isEmpty());
    QQmlComponent overlayComponent(&engine, QUrl::fromLocalFile(overlayPath));
    QVERIFY2(overlayComponent.isReady(),
             qPrintable(overlayComponent.errorString()));

    QQuickWindow window;
    window.resize(900, 600);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);

    TerminalPane *const firstPane = workspace->findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    TerminalController *const firstController =
        firstPane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    const QRectF firstGeometry(firstPane->position(), firstPane->size());

    // Attach the real QML component after the pane is latched. Initial QML
    // bindings settle without starting a transition, which keeps this
    // lifecycle test independent of Qt's global animation clock.
    Q_EMIT firstController->bell();
    QVERIFY(firstPane->bellRinging());
    QVERIFY(firstPane->bellBorderVisible());
    workspace->setBellBorderComponent(&overlayComponent);
    auto *const firstBorder = firstPane->findChild<QQuickItem *>(
        QStringLiteral("terminalBellBorder"), Qt::FindDirectChildrenOnly);
    QVERIFY(firstBorder != nullptr);
    QCOMPARE(firstBorder->parentItem(), firstPane);
    QVERIFY(!firstBorder->isEnabled());
    QVERIFY(firstBorder->isVisible());
    QCOMPARE(firstBorder->opacity(), qreal{1.0});
    QCOMPARE(firstBorder->position(), QPointF{});
    QCOMPARE(firstBorder->size(), firstPane->size());
    QCOMPARE(QRectF(firstPane->position(), firstPane->size()), firstGeometry);

    const TabId tabId = workspace->tabModel()->idAt(0);
    const PaneId firstId = workspace->tabModel()->entryAt(0)->activePaneId;
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    QTRY_COMPARE_WITH_TIMEOUT(workspace->findChildren<TerminalPane *>().size(),
                              2, 1000);
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane) secondPane = pane;
    }
    QVERIFY(secondPane != nullptr);
    auto *const secondBorder = secondPane->findChild<QQuickItem *>(
        QStringLiteral("terminalBellBorder"), Qt::FindDirectChildrenOnly);
    QVERIFY(secondBorder != nullptr);
    QVERIFY(secondBorder != firstBorder);
    QCOMPARE(secondBorder->parentItem(), secondPane);
    QVERIFY(!secondBorder->isEnabled());
    QVERIFY(!secondBorder->isVisible());
    QVERIFY(firstPane->bellRinging());
    QVERIFY(!secondPane->bellRinging());
    QVERIFY(firstBorder->isVisible());

    workspace->setBellBorderComponent(nullptr);
    QVERIFY(
        firstPane->findChild<QQuickItem *>(QStringLiteral("terminalBellBorder"),
                                           Qt::FindDirectChildrenOnly)
        == nullptr);
    QVERIFY(
        secondPane->findChild<QQuickItem *>(
            QStringLiteral("terminalBellBorder"), Qt::FindDirectChildrenOnly)
        == nullptr);
    workspace.reset();
    window.close();
}

void TerminalWorkspaceTest::readOnlyNaturalExitPromptsExactlyOnce()
{
    LaunchOptions options = baseOptions();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 0.4"),
    };
    options.hold = false;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    QSignalSpy ended(pane, &TerminalPane::sessionEnded);
    QPointer<TerminalPane> guardedPane(pane);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 3000);
    QVERIFY(!guardedPane.isNull());
    QVERIFY(guardedPane->isReadOnly());
    QTRY_COMPARE_WITH_TIMEOUT(confirmation.count(), 1, 1000);
    QCOMPARE(workspace.tabCount(), 1);
    QTest::qWait(150);
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(quit.count(), 0);

    workspace.confirmClose(closeConfirmationId(confirmation));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 0, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
}

void TerminalWorkspaceTest::queuesAndCorrelatesUnsafePasteConfirmations()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    workspace.splitRight();
    const QList<TerminalPane *> panes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 2);
    TerminalPane *const firstPane = panes.at(0);
    TerminalPane *const secondPane = panes.at(1);

    TerminalController *const firstController =
        firstPane->findChild<TerminalController *>();
    TerminalController *const secondController =
        secondPane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QVERIFY(secondController != nullptr);
    QSignalSpy firstPasted(firstController,
                           &TerminalController::pasteRequested);
    QSignalSpy secondPasted(secondController,
                            &TerminalController::pasteRequested);
    QSignalSpy firstConfirmed(firstController,
                              &TerminalController::confirmPasteRequested);
    QSignalSpy secondConfirmed(secondController,
                               &TerminalController::confirmPasteRequested);
    QSignalSpy firstCancelled(firstController,
                              &TerminalController::cancelPasteRequested);
    QSignalSpy secondCancelled(secondController,
                               &TerminalController::cancelPasteRequested);
    QSignalSpy previews(
        &workspace,
        &TerminalWorkspace::unsafePasteConfirmationRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::unsafePasteConfirmationResolved);

    const QString shared = QStringLiteral("shared\x1b[201~\n");
    Q_EMIT firstPane->unsafePasteRequested(11, shared, firstPane);
    QCOMPARE(previews.count(), 1);
    const quint64 sharedConfirmationId =
        previews.constFirst().at(0).toULongLong();
    QVERIFY(sharedConfirmationId != 0);
    QCOMPARE(previews.constFirst().at(1).toString(),
             QStringLiteral("shared␛[201~↵\n"));

    // Worker IDs are pane-local. Identical broad-action payloads share one
    // dialog, but every (pane, request ID) target remains correlated.
    Q_EMIT secondPane->unsafePasteRequested(11, shared, secondPane);
    QCOMPARE(previews.count(), 1);

    const QString queued = QStringLiteral("queued\ncommand");
    Q_EMIT firstPane->unsafePasteRequested(12, queued, firstPane);
    QCOMPARE(previews.count(), 1);

    workspace.confirmPaste(sharedConfirmationId);
    QCOMPARE(firstConfirmed.count(), 1);
    QCOMPARE(firstConfirmed.constFirst().constFirst().toULongLong(),
             quint64{11});
    QCOMPARE(secondConfirmed.count(), 1);
    QCOMPARE(secondConfirmed.constFirst().constFirst().toULongLong(),
             quint64{11});
    QCOMPARE(firstPasted.count(), 0);
    QCOMPARE(secondPasted.count(), 0);
    QCOMPARE(resolved.count(), 1);

    // A duplicate/stale response cannot consume the queued request before
    // its own dialog is shown.
    workspace.confirmPaste(sharedConfirmationId);
    QCOMPARE(firstConfirmed.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(previews.count(), 2, 1000);
    const quint64 queuedConfirmationId =
        previews.constLast().at(0).toULongLong();
    QVERIFY(queuedConfirmationId != 0);
    QVERIFY(queuedConfirmationId != sharedConfirmationId);
    QCOMPARE(previews.constLast().at(1).toString(),
             QStringLiteral("queued↵\ncommand"));

    workspace.cancelPaste(sharedConfirmationId);
    QCOMPARE(firstCancelled.count(), 0);
    workspace.cancelPaste(queuedConfirmationId);
    QCOMPARE(firstCancelled.count(), 1);
    QCOMPARE(firstCancelled.constFirst().constFirst().toULongLong(),
             quint64{12});
    QCOMPARE(secondCancelled.count(), 0);
    QCOMPARE(resolved.count(), 2);

    // Truncation is decided from the immutable original text, not the
    // expanded visible preview.
    const QString longPaste(241, u'x');
    Q_EMIT firstPane->unsafePasteRequested(13, longPaste, firstPane);
    QCOMPARE(previews.count(), 3);
    const quint64 longConfirmationId =
        previews.constLast().at(0).toULongLong();
    QCOMPARE(previews.constLast().at(1).toString(),
             QString(240, u'x') + QStringLiteral("…"));
    workspace.cancelPaste(longConfirmationId);
    QCOMPARE(firstCancelled.count(), 2);

    // Two same-pane requests cannot share a dialog because their worker IDs
    // are independently consumable. The second waits behind the first.
    const QString repeated = QStringLiteral("repeat\n");
    Q_EMIT firstPane->unsafePasteRequested(14, repeated, firstPane);
    Q_EMIT firstPane->unsafePasteRequested(15, repeated, firstPane);
    QCOMPARE(previews.count(), 4);
    const quint64 firstRepeatConfirmationId =
        previews.constLast().at(0).toULongLong();
    workspace.confirmPaste(firstRepeatConfirmationId);
    QCOMPARE(firstConfirmed.count(), 2);
    QCOMPARE(firstConfirmed.constLast().constFirst().toULongLong(),
             quint64{14});
    QTRY_COMPARE_WITH_TIMEOUT(previews.count(), 5, 1000);
    const quint64 secondRepeatConfirmationId =
        previews.constLast().at(0).toULongLong();
    workspace.cancelPaste(secondRepeatConfirmationId);
    QCOMPARE(firstCancelled.count(), 3);
    QCOMPARE(firstCancelled.constLast().constFirst().toULongLong(),
             quint64{15});

    // A held pane remains in the tree after its PTY exits. Its dead active
    // request is removed so a queued request for another live pane can be
    // reviewed without user interaction on the stale dialog.
    Q_EMIT firstPane->unsafePasteRequested(
        18, QStringLiteral("ended\n"), firstPane);
    Q_EMIT secondPane->unsafePasteRequested(
        19, QStringLiteral("survives\n"), secondPane);
    QCOMPARE(previews.count(), 6);
    const quint64 endedConfirmationId =
        previews.constLast().at(0).toULongLong();
    Q_EMIT firstPane->sessionEnded(firstPane, 0, 0);
    QCOMPARE(firstCancelled.count(), 4);
    QCOMPARE(firstCancelled.constLast().constFirst().toULongLong(),
             quint64{18});
    QCOMPARE(resolved.constLast().constFirst().toULongLong(),
             endedConfirmationId);
    QTRY_COMPARE_WITH_TIMEOUT(previews.count(), 7, 1000);
    const quint64 survivingConfirmationId =
        previews.constLast().at(0).toULongLong();
    workspace.cancelPaste(survivingConfirmationId);
    QCOMPARE(secondCancelled.count(), 1);
    QCOMPARE(secondCancelled.constLast().constFirst().toULongLong(),
             quint64{19});

    // Closing a target pane explicitly cancels its pending worker request and
    // resolves the visible dialog before the pane is destroyed.
    Q_EMIT secondPane->unsafePasteRequested(
        16, QStringLiteral("closing\n"), secondPane);
    QCOMPARE(previews.count(), 8);
    const quint64 closingConfirmationId =
        previews.constLast().at(0).toULongLong();
    QPointer<TerminalPane> closingPane(secondPane);
    Q_EMIT secondPane->requestClose();
    QCOMPARE(secondCancelled.count(), 2);
    QCOMPARE(secondCancelled.constLast().constFirst().toULongLong(),
             quint64{16});
    QCOMPARE(resolved.count(), 8);
    QCOMPARE(resolved.constLast().constFirst().toULongLong(),
             closingConfirmationId);

    // A worker rejection queued before removal can still reach the pane
    // during deleteLater teardown. It is cancelled without reopening a dead
    // target dialog.
    Q_EMIT secondPane->unsafePasteRequested(
        17, QStringLiteral("late\n"), secondPane);
    QCOMPARE(previews.count(), 8);
    QCOMPARE(secondCancelled.count(), 3);
    QCOMPARE(secondCancelled.constLast().constFirst().toULongLong(),
             quint64{17});
    QTRY_VERIFY_WITH_TIMEOUT(closingPane.isNull(), 1000);

    const int confirmations = firstConfirmed.count();
    const int cancellations = firstCancelled.count();
    workspace.confirmPaste(closingConfirmationId);
    workspace.cancelPaste(closingConfirmationId);
    QCOMPARE(firstConfirmed.count(), confirmations);
    QCOMPARE(firstCancelled.count(), cancellations);
}

void TerminalWorkspaceTest::ordersAndConfirmsTerminalClipboardWrites()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.clipboardWrite = TerminalClipboardAccess::Ask;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *const pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *const controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(controller->sessionStarted(), 1000);

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    clipboard->setText(QStringLiteral("unchanged"), QClipboard::Clipboard);

    QSignalSpy confirmations(
        &workspace,
        &TerminalWorkspace::terminalClipboardWriteConfirmationRequested);
    QSignalSpy resolved(
        &workspace,
        &TerminalWorkspace::terminalClipboardWriteConfirmationResolved);
    QSignalSpy runtimeUpdates(controller,
                              &TerminalController::runtimeOptionsRequested);

    const QByteArray firstBytes("first\0tail", 10);
    const TerminalClipboardWriteRequest first{
        .write =
            {
                .location = TerminalClipboardLocation::Standard,
                .contents = {{
                    .mime = QByteArrayLiteral("text/plain"),
                    .data = firstBytes,
                }},
            },
        .confirmationRequired = true,
    };
    const TerminalClipboardWriteRequest later{
        .write =
            {
                .location = TerminalClipboardLocation::Standard,
                .contents = {{
                    .mime = QByteArrayLiteral("text/plain"),
                    .data = QByteArrayLiteral("later"),
                }},
            },
        .confirmationRequired = false,
    };

    // A later allow-mode request remains behind an ask-mode request captured
    // before a config transition. The immutable binary payload is used for
    // the commit; the escaped preview is presentation-only.
    Q_EMIT pane->terminalClipboardWriteRequested(first, pane);
    Q_EMIT pane->terminalClipboardWriteRequested(later, pane);
    QTRY_COMPARE_WITH_TIMEOUT(confirmations.count(), 1, 1000);
    const quint64 firstConfirmationId =
        confirmations.constFirst().at(0).toULongLong();
    QVERIFY(firstConfirmationId != 0);
    QVERIFY(confirmations.constFirst().at(1).toString().contains(
        QStringLiteral("␀")));
    QCOMPARE(clipboard->text(QClipboard::Clipboard),
             QStringLiteral("unchanged"));

    workspace.confirmClipboardWrite(firstConfirmationId + 1, false);
    QCOMPARE(clipboard->text(QClipboard::Clipboard),
             QStringLiteral("unchanged"));
    workspace.confirmClipboardWrite(firstConfirmationId, true);
    QCOMPARE(clipboard->mimeData(QClipboard::Clipboard)
                 ->data(QStringLiteral("text/plain")),
             firstBytes);
    QCOMPARE(resolved.count(), 1);
    QVERIFY(!runtimeUpdates.isEmpty());
    QCOMPARE(qvariant_cast<TerminalSessionRuntimeOptions>(
                 runtimeUpdates.constLast().constFirst())
                 .clipboardWrite,
             TerminalClipboardAccess::Allow);

    QTRY_COMPARE_WITH_TIMEOUT(clipboard->mimeData(QClipboard::Clipboard)
                                  ->data(QStringLiteral("text/plain")),
                              QByteArrayLiteral("later"), 1000);

    // Reloading the configured Ask value replaces a remembered per-split
    // decision, exactly as the confirmation dialog promises.
    pane->applyRuntimeOptions(options);
    QVERIFY(!runtimeUpdates.isEmpty());
    QCOMPARE(qvariant_cast<TerminalSessionRuntimeOptions>(
                 runtimeUpdates.constLast().constFirst())
                 .clipboardWrite,
             TerminalClipboardAccess::Ask);

    Q_EMIT pane->terminalClipboardWriteRequested(first, pane);
    QTRY_COMPARE_WITH_TIMEOUT(confirmations.count(), 2, 1000);
    const quint64 deniedConfirmationId =
        confirmations.constLast().at(0).toULongLong();
    workspace.cancelClipboardWrite(deniedConfirmationId, true);
    QCOMPARE(clipboard->mimeData(QClipboard::Clipboard)
                 ->data(QStringLiteral("text/plain")),
             QByteArrayLiteral("later"));
    QCOMPARE(qvariant_cast<TerminalSessionRuntimeOptions>(
                 runtimeUpdates.constLast().constFirst())
                 .clipboardWrite,
             TerminalClipboardAccess::Deny);

    pane->applyRuntimeOptions(options);
    QCOMPARE(qvariant_cast<TerminalSessionRuntimeOptions>(
                 runtimeUpdates.constLast().constFirst())
                 .clipboardWrite,
             TerminalClipboardAccess::Ask);

    // Every representation is installed in one QMimeData ownership
    // transition and retains arbitrary bytes.
    const TerminalClipboardWriteRequest multiple{
        .write =
            {
                .location = TerminalClipboardLocation::Standard,
                .contents =
                    {
                        {
                            .mime = QByteArrayLiteral("text/plain"),
                            .data = QByteArrayLiteral("visible"),
                        },
                        {
                            .mime =
                                QByteArrayLiteral("application/x-ghostty-qt"),
                            .data = QByteArray("bin\0ary", 7),
                        },
                    },
            },
        .confirmationRequired = false,
    };
    Q_EMIT pane->terminalClipboardWriteRequested(multiple, pane);
    QTRY_COMPARE_WITH_TIMEOUT(
        clipboard->mimeData(QClipboard::Clipboard)
            ->data(QStringLiteral("application/x-ghostty-qt")),
        QByteArray("bin\0ary", 7), 1000);
    QCOMPARE(clipboard->mimeData(QClipboard::Clipboard)
                 ->data(QStringLiteral("text/plain")),
             QByteArrayLiteral("visible"));

    const TerminalClipboardWriteRequest clear{
        .write =
            {
                .location = TerminalClipboardLocation::Standard,
                .contents = {},
            },
        .confirmationRequired = false,
    };
    Q_EMIT pane->terminalClipboardWriteRequested(clear, pane);
    QTRY_VERIFY_WITH_TIMEOUT(
        [clipboard] {
            const QMimeData *const data =
                clipboard->mimeData(QClipboard::Clipboard);
            return data == nullptr
                || !data->hasFormat(QStringLiteral("text/plain"));
        }(),
        1000);

    // Authorization previews remain bounded and cannot use control,
    // directional-format, or line-separator characters to disguise content.
    TerminalClipboardWriteRequest hostileText = first;
    hostileText.write.contents[0].data =
        QByteArrayLiteral("safe\t") + QByteArray::fromHex("e280aee280a8");
    const int beforeTextPreview = confirmations.count();
    Q_EMIT pane->terminalClipboardWriteRequested(hostileText, pane);
    QTRY_COMPARE_WITH_TIMEOUT(confirmations.count(), beforeTextPreview + 1,
                              1000);
    const QList<QVariant> textPreview = confirmations.constLast();
    QVERIFY(textPreview.at(1).toString().contains(QStringLiteral("\\u{0009}")));
    QVERIFY(textPreview.at(1).toString().contains(QStringLiteral("\\u{202e}")));
    QVERIFY(textPreview.at(1).toString().contains(QStringLiteral("\\u{2028}")));
    workspace.cancelClipboardWrite(textPreview.at(0).toULongLong(), false);

    QByteArray hostileMime(4'096, 'm');
    hostileMime[0] = 'x';
    hostileMime[1] = '\n';
    hostileMime[2] = '\033';
    hostileMime[3] = static_cast<char>(0x85);
    const TerminalClipboardWriteRequest hostileFormat{
        .write =
            {
                .location = TerminalClipboardLocation::Standard,
                .contents = {{
                    .mime = hostileMime,
                    .data = QByteArrayLiteral("opaque"),
                }},
            },
        .confirmationRequired = true,
    };
    const int beforeFormatPreview = confirmations.count();
    Q_EMIT pane->terminalClipboardWriteRequested(hostileFormat, pane);
    QTRY_COMPARE_WITH_TIMEOUT(confirmations.count(), beforeFormatPreview + 1,
                              1000);
    const QList<QVariant> formatPreview = confirmations.constLast();
    const QString formatPreviewText = formatPreview.at(1).toString();
    QVERIFY(formatPreviewText.size() < 512);
    QVERIFY(formatPreviewText.contains(QStringLiteral("␊")));
    QVERIFY(formatPreviewText.contains(QStringLiteral("␛")));
    QVERIFY(formatPreviewText.contains(QStringLiteral("\\u{0085}")));
    QVERIFY(formatPreviewText.contains(QStringLiteral("…")));
    workspace.cancelClipboardWrite(formatPreview.at(0).toULongLong(), false);

    // Session exit invalidates both the visible decision and a later queued
    // write. A stale response cannot mutate the clipboard afterward.
    clipboard->setText(QStringLiteral("stable"), QClipboard::Clipboard);
    Q_EMIT pane->terminalClipboardWriteRequested(first, pane);
    Q_EMIT pane->terminalClipboardWriteRequested(later, pane);
    const int expectedSessionExitConfirmations = beforeFormatPreview + 2;
    QTRY_COMPARE_WITH_TIMEOUT(confirmations.count(),
                              expectedSessionExitConfirmations, 1000);
    const quint64 staleConfirmationId =
        confirmations.constLast().at(0).toULongLong();
    Q_EMIT pane->sessionEnded(pane, 0, 0);
    QCOMPARE(resolved.constLast().constFirst().toULongLong(),
             staleConfirmationId);
    workspace.confirmClipboardWrite(staleConfirmationId, false);
    QTest::qWait(20);
    QCOMPARE(clipboard->text(QClipboard::Clipboard), QStringLiteral("stable"));

    // Remembering a decision publishes a synchronous runtime update. If an
    // observer removes the source pane during that update, its still-live
    // deleteLater pointer must not authorize the already-popped request.
    clipboard->setText(QStringLiteral("reentrant-stable"),
                       QClipboard::Clipboard);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId paneId = workspace.tabModel()->entryAt(0)->activePaneId;
    const int beforeReentrantPreview = confirmations.count();
    Q_EMIT pane->terminalClipboardWriteRequested(first, pane);
    QTRY_COMPARE_WITH_TIMEOUT(confirmations.count(), beforeReentrantPreview + 1,
                              1000);
    const quint64 reentrantConfirmationId =
        confirmations.constLast().at(0).toULongLong();
    bool removedDuringRuntimeUpdate = false;
    const QMetaObject::Connection destructiveObserver =
        connect(controller, &TerminalController::runtimeOptionsRequested,
                &workspace, [&](const TerminalSessionRuntimeOptions &) {
                    if (removedDuringRuntimeUpdate) return;
                    removedDuringRuntimeUpdate = workspace.dispatchAction({
                        WorkspaceAction::ClosePane,
                        {tabId, paneId, 0},
                    });
                });
    workspace.confirmClipboardWrite(reentrantConfirmationId, true);
    disconnect(destructiveObserver);
    QVERIFY(removedDuringRuntimeUpdate);
    QCOMPARE(workspace.tabCount(), 0);
    QCOMPARE(clipboard->text(QClipboard::Clipboard),
             QStringLiteral("reentrant-stable"));
    QCOMPARE(resolved.constLast().constFirst().toULongLong(),
             reentrantConfirmationId);
}

void TerminalWorkspaceTest::performableTabChangeRequiresDifferentTarget()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("performable:ctrl+tab=next_tab"),
    });
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    QKeyEvent oneTab(QEvent::KeyPress, Qt::Key_Tab,
                     Qt::ControlModifier, QStringLiteral("\t"));
    QCoreApplication::sendEvent(pane, &oneTab);
    QCOMPARE(forwarded.count(), 1);
    QCOMPARE(workspace.currentIndex(), 0);

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    QCOMPARE(workspace.currentIndex(), 1);
    TerminalPane *activePane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate->isVisible()) {
            activePane = candidate;
        }
    }
    QVERIFY(activePane != nullptr);
    TerminalController *activeController =
        activePane->findChild<TerminalController *>();
    QVERIFY(activeController != nullptr);
    QSignalSpy activeForwarded(activeController,
                               &TerminalController::keyRequested);

    QKeyEvent twoTabs(QEvent::KeyPress, Qt::Key_Tab,
                      Qt::ControlModifier, QStringLiteral("\t"));
    QCoreApplication::sendEvent(activePane, &twoTabs);
    QCOMPARE(workspace.currentIndex(), 0);
    QCOMPARE(activeForwarded.count(), 0);
}

void TerminalWorkspaceTest::relativeTabActionsUseCurrentSelectionAndBroadFanout()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const CurrentTabProbe first = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe second = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe third = currentTabProbe(workspace);
    QVERIFY(first.pane && second.pane && third.pane);

    // The source surface identifies the window, but previous/next advance
    // from that window's current page. An inactive first-tab source must not
    // make this jump relative to the first tab itself.
    workspace.setCurrentIndex(2);
    QVERIFY(first.pane->executeConfiguredAction(
        QStringLiteral("next_tab")));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             first.tabId);
    QVERIFY(second.pane->executeConfiguredAction(
        QStringLiteral("previous_tab")));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             third.tabId);

    // Broad dispatch applies the relative action once per surface. Four
    // surfaces over three tabs therefore produce a net one-tab movement.
    workspace.setCurrentIndex(0);
    workspace.splitRight();
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 4);

    TerminalWorkspace otherWorkspace;
    QTRY_COMPARE_WITH_TIMEOUT(otherWorkspace.tabCount(), 1, 1000);
    otherWorkspace.newTab();
    otherWorkspace.setCurrentIndex(0);
    otherWorkspace.splitRight();
    QCOMPARE(otherWorkspace.findChildren<TerminalPane *>().size(), 3);

    GhosttyApplicationKeybindings bindings(options, false);
    bindings.registerWorkspace(&workspace);
    bindings.registerWorkspace(&otherWorkspace);
    bindings.dispatchBroadActions({QStringLiteral("next_tab")});
    QCOMPARE(workspace.currentIndex(), 1);      // 0 + 4 mod 3
    QCOMPARE(otherWorkspace.currentIndex(), 1); // 0 + 3 mod 2

    bindings.dispatchBroadActions({QStringLiteral("previous_tab")});
    QCOMPARE(workspace.currentIndex(), 0);
    QCOMPARE(otherWorkspace.currentIndex(), 0);
}

void TerminalWorkspaceTest::alwaysModePromptsForIdleShell()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.confirmCloseMode = ConfirmCloseMode::Always;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    QTest::qWait(350);

    workspace.requestWindowClose();
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(quit.count(), 0);
    workspace.cancelClose(closeConfirmationId(confirmation));
}

void TerminalWorkspaceTest::multiPaneShutdownGracePeriodsOverlap()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/workspace-shutdown-XXXXXX")));
    QVERIFY(directory.isValid());
    ResistantShellFixture resistantShell;
    QVERIFY(resistantShell.create(directory.path()));

    ShellEnvironment shell(resistantShell.encodedShellPath());
    LaunchOptions options = baseOptions();
    options.confirmCloseMode = ConfirmCloseMode::Always;
    options.workingDirectory = directory.path();
    TerminalWorkspace::setDefaultLaunchOptions(options);

    auto workspace = std::make_unique<TerminalWorkspace>();
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
    workspace->newTab();
    workspace->newTab();
    QCOMPARE(workspace->tabCount(), 3);

    // Every marker is created after the helper ignores SIGHUP. All workers
    // must therefore reach the two-second SIGKILL fallback during shutdown.
    QTRY_COMPARE_WITH_TIMEOUT(resistantShell.readyProcessCount(), 3, 5000);
    QSignalSpy confirmation(workspace.get(),
                            &TerminalWorkspace::closeConfirmationRequested);
    workspace->requestWindowClose();
    QCOMPARE(confirmation.count(), 1);

    QElapsedTimer elapsed;
    elapsed.start();
    workspace->confirmClose(closeConfirmationId(confirmation));
    workspace.reset();

    const qint64 shutdownMilliseconds = elapsed.elapsed();
    QVERIFY2(shutdownMilliseconds >= 1'500,
             "signal-resistant children did not exercise the shutdown grace period");
    QVERIFY2(shutdownMilliseconds < 4'500,
             "pane shutdown grace periods ran serially instead of concurrently");
}

void TerminalWorkspaceTest::closeTabModesUseStableOriginsAndPreserveFocus()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    {
        QQuickWindow window;
        window.resize(900, 600);
        TerminalWorkspace workspace;
        workspace.setParentItem(window.contentItem());
        workspace.setSize(window.size());
        window.show();
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

        const CurrentTabProbe first = currentTabProbe(workspace);
        workspace.newTab();
        const CurrentTabProbe second = currentTabProbe(workspace);
        workspace.newTab();
        const CurrentTabProbe third = currentTabProbe(workspace);
        workspace.newTab();
        const CurrentTabProbe fourth = currentTabProbe(workspace);
        QVERIFY(first.pane && second.pane && third.pane && fourth.pane);
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({first.tabId, second.tabId,
                                 third.tabId, fourth.tabId}));

        workspace.setCurrentIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(),
                                  first.pane.data(), 1000);
        QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);

        WorkspaceActionContext mismatched;
        mismatched.tabId = first.tabId;
        mismatched.paneId = second.paneId;
        mismatched.closeTabMode = CloseTabMode::Right;
        QVERIFY(!workspace.dispatchAction(
            {WorkspaceAction::CloseTab, mismatched}));
        QCOMPARE(workspace.tabCount(), 4);

        // The source identity survives a row move. Right is evaluated from
        // the source's current visual position, not its old index or the
        // selected first tab.
        QVERIFY(workspace.dispatchAction({
            WorkspaceAction::MoveTab,
            {second.tabId, second.paneId, 1},
        }));
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({first.tabId, third.tabId,
                                 second.tabId, fourth.tabId}));
        QVERIFY(second.pane->executeConfiguredAction(
            QStringLiteral("close_tab:right")));
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({first.tabId, third.tabId, second.tabId}));
        QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
                 first.tabId);
        QCOMPARE(window.activeFocusItem(), first.pane.data());
        QTRY_VERIFY_WITH_TIMEOUT(fourth.pane.isNull(), 1000);
        QVERIFY(third.pane && second.pane);
        QCOMPARE(quit.count(), 0);

        // A valid rightmost no-op is still performed and does not assess the
        // protected source when its target set is empty.
        QVERIFY(second.pane->executeConfiguredAction(
            QStringLiteral("toggle_readonly")));
        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QVERIFY(second.pane->executeConfiguredAction(
            QStringLiteral("close_tab:right")));
        QCOMPARE(confirmation.count(), 0);
        QCOMPARE(workspace.tabCount(), 3);
        QCOMPARE(window.activeFocusItem(), first.pane.data());
    }

    {
        QQuickWindow window;
        window.resize(900, 600);
        TerminalWorkspace workspace;
        workspace.setParentItem(window.contentItem());
        workspace.setSize(window.size());
        window.show();
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

        const CurrentTabProbe first = currentTabProbe(workspace);
        workspace.newTab();
        const CurrentTabProbe second = currentTabProbe(workspace);
        workspace.newTab();
        const CurrentTabProbe third = currentTabProbe(workspace);
        workspace.newTab();
        const CurrentTabProbe fourth = currentTabProbe(workspace);
        QVERIFY(first.pane && second.pane && third.pane && fourth.pane);

        workspace.setCurrentIndex(1);
        QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(),
                                  second.pane.data(), 1000);
        QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
        QVERIFY(second.pane->executeConfiguredAction(
            QStringLiteral("close_tab:other")));
        QCOMPARE(tabIds(workspace), QVector<TabId>({second.tabId}));
        QCOMPARE(workspace.currentIndex(), 0);
        QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId,
                 second.paneId);
        QCOMPARE(window.activeFocusItem(), second.pane.data());
        QTRY_VERIFY_WITH_TIMEOUT(first.pane.isNull(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(third.pane.isNull(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(fourth.pane.isNull(), 1000);
        QCOMPARE(quit.count(), 0);

        // Other on a sole tab is also a successful no-op and never widens to
        // the containing-window shutdown path.
        QVERIFY(second.pane->executeConfiguredAction(
            QStringLiteral("toggle_readonly")));
        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QVERIFY(second.pane->executeConfiguredAction(
            QStringLiteral("close_tab:other")));
        QCOMPARE(confirmation.count(), 0);
        QCOMPARE(workspace.tabCount(), 1);
        QCOMPARE(quit.count(), 0);
    }
}

void TerminalWorkspaceTest::closeTabBatchConfirmationKeepsStableTargets()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Always;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    {
        TerminalWorkspace workspace;
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        const CurrentTabProbe first = currentTabProbe(workspace);
        workspace.newTab();
        const CurrentTabProbe second = currentTabProbe(workspace);
        workspace.newTab();
        const CurrentTabProbe third = currentTabProbe(workspace);
        workspace.newTab();
        const CurrentTabProbe fourth = currentTabProbe(workspace);
        QVERIFY(first.pane && second.pane && third.pane && fourth.pane);
        workspace.setCurrentIndex(1);

        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);

        QVERIFY(second.pane->executeConfiguredAction(
            QStringLiteral("close_tab:other")));
        QCOMPARE(confirmation.count(), 1);
        QCOMPARE(workspace.tabCount(), 4);
        workspace.cancelClose(closeConfirmationId(confirmation));
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({first.tabId, second.tabId,
                                 third.tabId, fourth.tabId}));

        QVERIFY(second.pane->executeConfiguredAction(
            QStringLiteral("close_tab:right")));
        QCOMPARE(confirmation.count(), 2);
        workspace.cancelClose(closeConfirmationId(confirmation));
        QCOMPARE(workspace.tabCount(), 4);

        QVERIFY(second.pane->executeConfiguredAction(
            QStringLiteral("close_tab:right")));
        QCOMPARE(confirmation.count(), 3);

        // Freeze C and D as the target set, then move D left of the source,
        // move non-target A right of it, and insert E to its right. Confirming
        // must still close exactly the original stable IDs.
        QVERIFY(workspace.dispatchAction({
            WorkspaceAction::MoveTab,
            {fourth.tabId, fourth.paneId, 1},
        }));
        QVERIFY(workspace.dispatchAction({
            WorkspaceAction::MoveTab,
            {first.tabId, first.paneId, 2},
        }));
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({fourth.tabId, second.tabId,
                                 third.tabId, first.tabId}));
        workspace.newTab();
        const CurrentTabProbe inserted = currentTabProbe(workspace);
        QVERIFY(inserted.pane);
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({fourth.tabId, second.tabId,
                                 inserted.tabId, third.tabId, first.tabId}));

        workspace.confirmClose(closeConfirmationId(confirmation));
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({second.tabId, inserted.tabId, first.tabId}));
        QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
                 inserted.tabId);
        QVERIFY(second.pane && inserted.pane && first.pane);
        QTRY_VERIFY_WITH_TIMEOUT(third.pane.isNull(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(fourth.pane.isNull(), 1000);
        QCOMPARE(quit.count(), 0);
    }

    {
        TerminalWorkspace workspace;
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        const CurrentTabProbe first = currentTabProbe(workspace);
        workspace.newTab();
        workspace.newTab();
        workspace.setCurrentIndex(0);

        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QSignalSpy resolved(
            &workspace, &TerminalWorkspace::closeConfirmationResolved);
        QVERIFY(first.pane->executeConfiguredAction(
            QStringLiteral("close_tab:right")));
        QCOMPARE(confirmation.count(), 1);
        QCOMPARE(workspace.tabCount(), 3);

        LaunchOptions reloadedOptions = options;
        reloadedOptions.confirmCloseMode = ConfirmCloseMode::Never;
        workspace.applyLaunchOptions(reloadedOptions);
        QCOMPARE(resolved.count(), 1);
        QCOMPARE(tabIds(workspace), QVector<TabId>({first.tabId}));
    }
}

void TerminalWorkspaceTest::closeTabResponsesUseStableConfirmationIds()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const CurrentTabProbe first = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe second = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe third = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe fourth = currentTabProbe(workspace);
    QVERIFY(first.pane && second.pane && third.pane && fourth.pane);
    QVERIFY(second.pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));
    QVERIFY(third.pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QSignalSpy confirmation(
        &workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::closeConfirmationResolved);
    QVERIFY(second.pane->executeConfiguredAction(
        QStringLiteral("close_tab:this")));
    QCOMPARE(confirmation.count(), 1);
    const quint64 firstConfirmationId = closeConfirmationId(confirmation);
    QVERIFY(firstConfirmationId != 0);

    // Making the first target safe auto-resolves and commits that request.
    // A response queued by its old dialog must not affect the next request.
    QVERIFY(second.pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));
    QCOMPARE(tabIds(workspace),
             QVector<TabId>({first.tabId, third.tabId, fourth.tabId}));
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(closeConfirmationId(resolved), firstConfirmationId);
    QTRY_VERIFY_WITH_TIMEOUT(second.pane.isNull(), 1000);

    QVERIFY(first.pane->executeConfiguredAction(
        QStringLiteral("close_tab:right")));
    QCOMPARE(confirmation.count(), 2);
    const quint64 secondConfirmationId = closeConfirmationId(confirmation);
    QVERIFY(secondConfirmationId != 0);
    QVERIFY(secondConfirmationId != firstConfirmationId);

    workspace.confirmClose(firstConfirmationId);
    workspace.cancelClose(firstConfirmationId);
    QCOMPARE(tabIds(workspace),
             QVector<TabId>({first.tabId, third.tabId, fourth.tabId}));
    QCOMPARE(resolved.count(), 1);

    workspace.confirmClose(secondConfirmationId);
    QCOMPARE(tabIds(workspace), QVector<TabId>({first.tabId}));
    QCOMPARE(resolved.count(), 2);
    QCOMPARE(closeConfirmationId(resolved), secondConfirmationId);
    QVERIFY(first.pane);
    QTRY_VERIFY_WITH_TIMEOUT(third.pane.isNull(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(fourth.pane.isNull(), 1000);
}

void TerminalWorkspaceTest::closeTabBatchRejectsReentrantTopologyChanges()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const CurrentTabProbe first = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe second = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe third = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe fourth = currentTabProbe(workspace);
    QVERIFY(first.pane && second.pane && third.pane && fourth.pane);
    workspace.setCurrentIndex(1);

    int rowsRemovedCount = 0;
    bool selectionStayedCoherent = true;
    bool nestedMoveAccepted = true;
    connect(workspace.tabModel(), &QAbstractItemModel::rowsRemoved,
            &workspace,
            [&](const QModelIndex &, int, int) {
        ++rowsRemovedCount;
        const int current = workspace.currentIndex();
        selectionStayedCoherent = selectionStayedCoherent
            && current >= 0 && current < workspace.tabModel()->count()
            && workspace.tabModel()->idAt(current) == second.tabId;
        if (rowsRemovedCount == 1) {
            nestedMoveAccepted = workspace.dispatchAction({
                WorkspaceAction::MoveTab,
                {second.tabId, second.paneId, 1},
            });
        }
    });

    QVERIFY(second.pane->executeConfiguredAction(
        QStringLiteral("close_tab:other")));
    QCOMPARE(rowsRemovedCount, 3);
    QVERIFY(selectionStayedCoherent);
    QVERIFY(!nestedMoveAccepted);
    QCOMPARE(tabIds(workspace), QVector<TabId>({second.tabId}));
    QCOMPARE(workspace.currentIndex(), 0);
    QVERIFY(second.pane);
    QTRY_VERIFY_WITH_TIMEOUT(first.pane.isNull(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(third.pane.isNull(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(fourth.pane.isNull(), 1000);
}

void TerminalWorkspaceTest::pendingCloseTargetsPruneBeforeModelPublication()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const CurrentTabProbe first = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe source = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe third = currentTabProbe(workspace);
    QVERIFY(first.pane && source.pane && third.pane);
    QVERIFY(first.pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));
    QVERIFY(third.pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QSignalSpy confirmation(
        &workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::closeConfirmationResolved);
    QVERIFY(source.pane->executeConfiguredAction(
        QStringLiteral("close_tab:other")));
    QCOMPARE(confirmation.count(), 1);
    const quint64 confirmationId = closeConfirmationId(confirmation);

    bool finalTitlesSawResolution = false;
    connect(&workspace, &TerminalWorkspace::tabTitlesChanged,
            &workspace, [&] {
        if (workspace.tabCount() == 1) {
            finalTitlesSawResolution = resolved.count() == 1;
        }
    });

    // Suppress live policy reevaluation so each now-safe target is removed by
    // its own action while the original multi-target dialog remains pending.
    {
        QSignalBlocker blocker(first.pane);
        QVERIFY(first.pane->executeConfiguredAction(
            QStringLiteral("toggle_readonly")));
    }
    QVERIFY(first.pane->executeConfiguredAction(
        QStringLiteral("close_tab:this")));
    QCOMPARE(tabIds(workspace),
             QVector<TabId>({source.tabId, third.tabId}));
    QCOMPARE(resolved.count(), 0);

    {
        QSignalBlocker blocker(third.pane);
        QVERIFY(third.pane->executeConfiguredAction(
            QStringLiteral("toggle_readonly")));
    }
    QVERIFY(third.pane->executeConfiguredAction(
        QStringLiteral("close_tab:this")));
    QCOMPARE(tabIds(workspace), QVector<TabId>({source.tabId}));
    QVERIFY(finalTitlesSawResolution);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(closeConfirmationId(resolved), confirmationId);
    QVERIFY(source.pane);
    QTRY_VERIFY_WITH_TIMEOUT(first.pane.isNull(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(third.pane.isNull(), 1000);
}

void TerminalWorkspaceTest::closeResponseDefersDuringBatchMutation()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const CurrentTabProbe first = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe source = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe third = currentTabProbe(workspace);
    QVERIFY(first.pane && source.pane && third.pane);
    QVERIFY(third.pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QSignalSpy confirmation(
        &workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::closeConfirmationResolved);
    QVERIFY(source.pane->executeConfiguredAction(
        QStringLiteral("close_tab:other")));
    QCOMPARE(confirmation.count(), 1);
    const quint64 confirmationId = closeConfirmationId(confirmation);

    bool responseWasDeferred = false;
    connect(workspace.tabModel(), &QAbstractItemModel::rowsRemoved,
            &workspace,
            [&](const QModelIndex &, int, int) {
        if (workspace.tabCount() != 2) return;
        workspace.confirmClose(confirmationId);
        responseWasDeferred =
            tabIds(workspace)
                == QVector<TabId>({source.tabId, third.tabId})
            && resolved.count() == 0;
    });

    // Removing the safe member emits rowsRemoved while the batch mutation
    // guard is held. A synchronous QML response is queued, not nested.
    QVERIFY(first.pane->executeConfiguredAction(
        QStringLiteral("close_tab:this")));
    QVERIFY(responseWasDeferred);
    QCOMPARE(tabIds(workspace),
             QVector<TabId>({source.tabId, third.tabId}));
    QCOMPARE(resolved.count(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    QCOMPARE(tabIds(workspace), QVector<TabId>({source.tabId}));
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(closeConfirmationId(resolved), confirmationId);
    QVERIFY(source.pane);
    QTRY_VERIFY_WITH_TIMEOUT(first.pane.isNull(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(third.pane.isNull(), 1000);
}

void TerminalWorkspaceTest::naturalTabExitPrunesPendingBatchTarget()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/close-tab-natural-exit-XXXXXX")));
    QVERIFY(directory.isValid());

    const QString shellPath = directory.filePath(QStringLiteral("long-shell"));
    QFile shellFile(shellPath);
    QVERIFY(shellFile.open(QIODevice::WriteOnly));
    const QByteArray shellScript = QByteArrayLiteral(
        "#!/bin/sh\n"
        "exec /bin/sleep 30\n");
    QCOMPARE(shellFile.write(shellScript), shellScript.size());
    shellFile.close();
    QVERIFY(shellFile.setPermissions(QFileDevice::ReadOwner
                                     | QFileDevice::WriteOwner
                                     | QFileDevice::ExeOwner));

    const QString readyPath = directory.filePath(QStringLiteral("ready"));
    const QString releasePath = directory.filePath(QStringLiteral("release"));
    ShellEnvironment shell(QFile::encodeName(shellPath));
    LaunchOptions options = baseOptions();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(": > \"$1\"; while [ ! -e \"$2\" ]; do sleep 0.01; done"),
        QStringLiteral("close-tab-waiter"),
        readyPath,
        releasePath,
    };
    options.hold = false;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.workingDirectory = directory.path();
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const CurrentTabProbe first = currentTabProbe(workspace);
    QVERIFY(first.pane);
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(readyPath), 3000);

    // New tabs intentionally clear the explicit program and start $SHELL.
    workspace.newTab();
    const CurrentTabProbe second = currentTabProbe(workspace);
    workspace.newTab();
    const CurrentTabProbe third = currentTabProbe(workspace);
    QVERIFY(second.pane && third.pane);
    QVERIFY(third.pane->executeConfiguredAction(
        QStringLiteral("toggle_readonly")));

    QSignalSpy confirmation(
        &workspace, &TerminalWorkspace::closeConfirmationRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::closeConfirmationResolved);
    QVERIFY(second.pane->executeConfiguredAction(
        QStringLiteral("close_tab:other")));
    QCOMPARE(confirmation.count(), 1);
    const quint64 confirmationId = closeConfirmationId(confirmation);

    QFile releaseFile(releasePath);
    QVERIFY(releaseFile.open(QIODevice::WriteOnly));
    releaseFile.close();
    QTRY_VERIFY_WITH_TIMEOUT(first.pane.isNull(), 3000);
    QCOMPARE(tabIds(workspace),
             QVector<TabId>({second.tabId, third.tabId}));
    QCOMPARE(resolved.count(), 0);

    workspace.confirmClose(confirmationId);
    QCOMPARE(tabIds(workspace), QVector<TabId>({second.tabId}));
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(closeConfirmationId(resolved), confirmationId);
    QVERIFY(second.pane);
    QTRY_VERIFY_WITH_TIMEOUT(third.pane.isNull(), 1000);
}

void TerminalWorkspaceTest::broadCloseTabModesUseFirstStableSource()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    const auto exercise = [&](QStringView action,
                              GhosttyKeybindFlags flags,
                              Qt::Key key,
                              quint32 codepoint,
                              QChar controlCharacter) {
        LaunchOptions exerciseOptions = options;
        GhosttyKeybindConfig config;
        config.root = {GhosttyKeybindDefinition{
            .sequence = {GhosttyKeybindTrigger{
                .kind = GhosttyKeybindKeyKind::Unicode,
                .unicodeCodepoint = codepoint,
                .modifiers = GhosttyKeybindCtrl,
            }},
            .actions = {action.toString()},
            .flags = flags,
        }};
        exerciseOptions.keybindSource =
            GhosttyKeybindSource::structured(std::move(config));
        TerminalWorkspace::setDefaultLaunchOptions(exerciseOptions);

        TerminalWorkspace workspace;
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        const CurrentTabProbe first = currentTabProbe(workspace);
        QVERIFY(first.pane);
        workspace.splitRight();
        QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 2);
        workspace.newTab();
        const CurrentTabProbe second = currentTabProbe(workspace);
        QVERIFY(second.pane->executeConfiguredAction(
            QStringLiteral("toggle_readonly")));
        workspace.newTab();
        const CurrentTabProbe third = currentTabProbe(workspace);
        QVERIFY(second.pane && third.pane);
        workspace.setCurrentIndex(2);

        GhosttyApplicationKeybindings bindings(exerciseOptions, false);
        bindings.registerWorkspace(&workspace);
        QSignalSpy confirmation(
            &workspace, &TerminalWorkspace::closeConfirmationRequested);
        QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
        TerminalController *const controller =
            third.pane->findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        QSignalSpy forwarded(controller, &TerminalController::keyRequested);
        QKeyEvent press(QEvent::KeyPress, key, Qt::ControlModifier,
                        QString(controlCharacter));
        QCoreApplication::sendEvent(third.pane, &press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::ControlModifier);
        QCoreApplication::sendEvent(third.pane, &release);
        QCOMPARE(confirmation.count(), 1);
        QCOMPARE(forwarded.count(), 0);
        QCOMPARE(workspace.tabCount(), 3);

        workspace.confirmClose(closeConfirmationId(confirmation));
        QCOMPARE(tabIds(workspace), QVector<TabId>({first.tabId}));
        QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
                 first.tabId);
        QCOMPARE(quit.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(second.pane.isNull(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(third.pane.isNull(), 1000);

        // Duplicate split sources and valid no-target fanout are harmless.
        bindings.dispatchBroadActions({QStringLiteral("close_tab:other")});
        bindings.dispatchBroadActions({QStringLiteral("close_tab:right")});
        QCOMPARE(confirmation.count(), 1);
        QCOMPARE(workspace.tabCount(), 1);
        QCOMPARE(quit.count(), 0);
    };

    exercise(QStringLiteral("close_tab:other"),
             GhosttyKeybindFlags{.all = true},
             Qt::Key_O, 'o', QChar(0x0f));
    exercise(QStringLiteral("close_tab:right"),
             GhosttyKeybindFlags{.global = true},
             Qt::Key_R, 'r', QChar(0x12));
}

void TerminalWorkspaceTest::closeTabBatchShutdownGracePeriodsOverlap()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/close-tab-shutdown-XXXXXX")));
    QVERIFY(directory.isValid());
    ResistantShellFixture resistantShell;
    QVERIFY(resistantShell.create(directory.path()));

    ShellEnvironment shell(resistantShell.encodedShellPath());
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Always;
    options.workingDirectory = directory.path();
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const CurrentTabProbe source = currentTabProbe(workspace);
    QVector<QPointer<TerminalPane>> targets;
    for (int index = 0; index < 3; ++index) {
        workspace.newTab();
        targets.append(currentTabProbe(workspace).pane);
    }
    workspace.setCurrentIndex(0);
    QTRY_COMPARE_WITH_TIMEOUT(resistantShell.readyProcessCount(), 3, 5000);

    QSignalSpy confirmation(
        &workspace, &TerminalWorkspace::closeConfirmationRequested);
    QVERIFY(source.pane->executeConfiguredAction(
        QStringLiteral("close_tab:right")));
    QCOMPARE(confirmation.count(), 1);

    QElapsedTimer elapsed;
    elapsed.start();
    workspace.confirmClose(closeConfirmationId(confirmation));
    QCOMPARE(workspace.tabCount(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(std::ranges::all_of(
        targets, [](const QPointer<TerminalPane> &pane) {
            return pane.isNull();
        }), 6000);

    const qint64 shutdownMilliseconds = elapsed.elapsed();
    QVERIFY2(shutdownMilliseconds >= 1'500,
             "signal-resistant tab workers did not exercise the grace period");
    QVERIFY2(shutdownMilliseconds < 4'500,
             "tab shutdown grace periods ran serially instead of concurrently");
}

void TerminalWorkspaceTest::rootApplicationBindingPrecedesActiveTable()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    const auto unicode = [](quint32 codepoint, quint8 modifiers = 0) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = modifiers,
        };
    };
    GhosttyKeybindConfig config;
    config.root = {
        GhosttyKeybindDefinition{
            .sequence = {unicode('m', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("activate_key_table:modal")},
        },
        GhosttyKeybindDefinition{
            .sequence = {unicode('r', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("reload_config")},
        },
        GhosttyKeybindDefinition{
            .sequence = {unicode('i', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("ignore")},
        },
        GhosttyKeybindDefinition{
            .sequence = {unicode('g', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("ignore")},
            .flags = GhosttyKeybindFlags{.global = true},
        },
        GhosttyKeybindDefinition{
            .sequence = {unicode('a', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("ignore")},
            .flags = GhosttyKeybindFlags{.all = true},
        },
    };
    config.tables = {GhosttyKeybindTable{
        .name = QStringLiteral("modal"),
        .bindings = {
            GhosttyKeybindDefinition{
                .sequence = {unicode('r', GhosttyKeybindCtrl)},
                .actions = {QStringLiteral("new_tab")},
            },
            GhosttyKeybindDefinition{
                .sequence = {unicode('i', GhosttyKeybindCtrl)},
                .actions = {QStringLiteral("new_tab")},
            },
            GhosttyKeybindDefinition{
                .sequence = {unicode('g', GhosttyKeybindCtrl)},
                .actions = {QStringLiteral("new_tab")},
            },
            GhosttyKeybindDefinition{
                .sequence = {unicode('a', GhosttyKeybindCtrl)},
                .actions = {QStringLiteral("new_tab")},
            },
        },
    }};
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    GhosttyApplicationKeybindings applicationBindings(options, false);
    applicationBindings.registerWorkspace(&workspace);
    QSignalSpy reload(
        &applicationBindings,
        &GhosttyApplicationKeybindings::applicationActionRequested);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    QKeyEvent activate(QEvent::KeyPress, Qt::Key_M,
                       Qt::ControlModifier, QString(QChar(0x0d)));
    QCoreApplication::sendEvent(pane, &activate);
    QCOMPARE(pane->activeKeyTables(),
             QStringList({QStringLiteral("modal")}));

    QKeyEvent rootApp(QEvent::KeyPress, Qt::Key_R,
                      Qt::ControlModifier, QString(QChar(0x12)));
    QCoreApplication::sendEvent(pane, &rootApp);
    QKeyEvent rootAppRelease(QEvent::KeyRelease, Qt::Key_R,
                             Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &rootAppRelease);
    QCOMPARE(reload.count(), 1);
    QCOMPARE(forwarded.count(), 0);
    QCOMPARE(workspace.tabCount(), 1);
    QCOMPARE(pane->activeKeyTables(),
             QStringList({QStringLiteral("modal")}));

    const auto exerciseIgnore = [&](Qt::Key key, QChar text,
                                    int expectedActionCount) {
        const int beforeForwarded = forwarded.count();
        QKeyEvent press(QEvent::KeyPress, key, Qt::ControlModifier,
                        QString(text));
        QCoreApplication::sendEvent(pane, &press);
        QCOMPARE(reload.count(), expectedActionCount);
        QCOMPARE(qvariant_cast<ApplicationAction>(
                     reload.constLast().constFirst()),
                 ApplicationAction::Ignore);
        QCOMPARE(workspace.tabCount(), 1);
        QCOMPARE(forwarded.count(), beforeForwarded);

        // Ignore is press-only in Ghostty's input effects. Unlike other root
        // and broad actions, it deliberately leaves the release available to
        // normal terminal encoding.
        QKeyEvent release(QEvent::KeyRelease, key, Qt::ControlModifier);
        QCoreApplication::sendEvent(pane, &release);
        QCOMPARE(forwarded.count(), beforeForwarded + 1);
    };
    exerciseIgnore(Qt::Key_I, QChar(0x09), 2);
    exerciseIgnore(Qt::Key_G, QChar(0x07), 3);
    exerciseIgnore(Qt::Key_A, QChar(0x01), 4);
}

void TerminalWorkspaceTest::
    windowNavigationRetainsSurfaceScopeAndBroadFanout()
{
    qRegisterMetaType<WindowNavigationAction>();

    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    GhosttyKeybindConfig config;
    config.root = {GhosttyKeybindDefinition{
        .sequence = {GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = 'n',
            .modifiers = GhosttyKeybindCtrl,
        }},
        .actions = {QStringLiteral("goto_window:next")},
        .flags = GhosttyKeybindFlags{.all = true},
    }};
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    GhosttyApplicationKeybindings applicationBindings(options, false);
    applicationBindings.registerWorkspace(&workspace);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    workspace.splitRight();
    workspace.newTab();
    const QList<TerminalPane *> panes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 3);
    TerminalPane *const source = panes.constFirst();
    QVERIFY(source != nullptr);

    QSignalSpy navigation(
        &workspace, &TerminalWorkspace::windowNavigationRequested);
    QSignalSpy application(
        &applicationBindings,
        &GhosttyApplicationKeybindings::applicationActionRequested);
    TerminalController *const terminal =
        source->findChild<TerminalController *>();
    QVERIFY(terminal != nullptr);
    QSignalSpy forwarded(terminal, &TerminalController::keyRequested);

    QVERIFY(source->executeConfiguredAction(
        QStringLiteral("goto_window:previous")));
    QCOMPARE(navigation.count(), 1);
    QCOMPARE(
        qvariant_cast<WindowNavigationAction>(
            navigation.constFirst().at(0)),
        WindowNavigationAction::Previous);
    QVERIFY(
        qvariant_cast<PaneId>(navigation.constFirst().at(1)).isValid());
    navigation.clear();

    QKeyEvent press(QEvent::KeyPress, Qt::Key_N, Qt::ControlModifier,
                    QString(QChar(0x0e)));
    QCoreApplication::sendEvent(source, &press);
    QKeyEvent release(
        QEvent::KeyRelease, Qt::Key_N, Qt::ControlModifier);
    QCoreApplication::sendEvent(source, &release);

    QCOMPARE(navigation.count(), panes.size());
    QSet<PaneId> sources;
    for (const QList<QVariant> &arguments : navigation) {
        QCOMPARE(
            qvariant_cast<WindowNavigationAction>(arguments.at(0)),
            WindowNavigationAction::Next);
        const PaneId paneId =
            qvariant_cast<PaneId>(arguments.at(1));
        QVERIFY(paneId.isValid());
        sources.insert(paneId);
    }
    QCOMPARE(sources.size(), panes.size());
    QCOMPARE(application.count(), 0);
    QCOMPARE(forwarded.count(), 0);
}

void TerminalWorkspaceTest::broadBindingsReachInactivePanesAndIgnoreLocalFlags()
{
    ShellEnvironment shell;
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    const auto unicode = [](quint32 codepoint, quint8 modifiers = 0) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = modifiers,
        };
    };
    GhosttyKeybindConfig config;
    config.root = {
        GhosttyKeybindDefinition{
            .sequence = {unicode('g', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("increase_font_size:2")},
            .flags = GhosttyKeybindFlags{
                .consumed = false,
                .global = true,
                .performable = true,
            },
        },
        GhosttyKeybindDefinition{
            .sequence = {unicode('r', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("reload_config")},
            .flags = GhosttyKeybindFlags{.all = true},
        },
        GhosttyKeybindDefinition{
            .sequence = {unicode('p', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("copy_to_clipboard")},
            .flags = GhosttyKeybindFlags{
                .consumed = false,
                .all = true,
                .performable = true,
            },
        },
        GhosttyKeybindDefinition{
            .sequence = {unicode('x', GhosttyKeybindCtrl), unicode('n')},
            .actions = {QStringLiteral("new_tab")},
        },
    };
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    GhosttyApplicationKeybindings applicationBindings(options, false);
    applicationBindings.registerWorkspace(&workspace);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);

    const QList<TerminalPane *> panes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 2);
    TerminalPane *activePane = nullptr;
    TerminalPane *inactivePane = nullptr;
    for (TerminalPane *pane : panes) {
        if (pane->isVisible()) activePane = pane;
        else inactivePane = pane;
    }
    QVERIFY(activePane != nullptr);
    QVERIFY(inactivePane != nullptr);
    QCOMPARE(activePane->fontPointSize(), 12.0);
    QCOMPARE(inactivePane->fontPointSize(), 12.0);
    TerminalController *controller =
        activePane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    TerminalController *inactiveController =
        inactivePane->findChild<TerminalController *>();
    QVERIFY(inactiveController != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy activeSequenceResolution(
        controller, &TerminalController::sequenceResolutionRequested);
    QSignalSpy inactiveSequenceResolution(
        inactiveController, &TerminalController::sequenceResolutionRequested);
    QSignalSpy activeCopy(
        controller, &TerminalController::copyActionRequested);
    QSignalSpy inactiveCopy(
        inactiveController, &TerminalController::copyActionRequested);

    // An external all/global action can end sequences staged in panes that no
    // longer have focus. It must reset both the matcher and worker token.
    QKeyEvent activeLeader(QEvent::KeyPress, Qt::Key_X,
                           Qt::ControlModifier, QString(QChar(0x18)));
    QKeyEvent inactiveLeader(QEvent::KeyPress, Qt::Key_X,
                             Qt::ControlModifier, QString(QChar(0x18)));
    QCoreApplication::sendEvent(activePane, &activeLeader);
    QCoreApplication::sendEvent(inactivePane, &inactiveLeader);
    QCOMPARE(activeSequenceResolution.count(), 0);
    QCOMPARE(inactiveSequenceResolution.count(), 0);
    applicationBindings.dispatchBroadActions(
        {QStringLiteral("end_key_sequence")});
    QCOMPARE(activeSequenceResolution.count(), 1);
    QCOMPARE(inactiveSequenceResolution.count(), 1);
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 activeSequenceResolution.front().at(1)),
             TerminalSequenceResolution::Flush);
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 inactiveSequenceResolution.front().at(1)),
             TerminalSequenceResolution::Flush);

    // global implies all. The local unconsumed/performable flags have no
    // effect, and the inactive tab's surface receives the action too.
    QKeyEvent global(QEvent::KeyPress, Qt::Key_G,
                     Qt::ControlModifier, QString(QChar(0x07)));
    QCoreApplication::sendEvent(activePane, &global);
    QKeyEvent globalRelease(QEvent::KeyRelease, Qt::Key_G,
                            Qt::ControlModifier);
    QCoreApplication::sendEvent(activePane, &globalRelease);
    QCOMPARE(activePane->fontPointSize(), 14.0);
    QCOMPARE(inactivePane->fontPointSize(), 14.0);
    QCOMPARE(forwarded.count(), 0);

    // An app-scoped all: action runs once in the root pre-pass rather than
    // once per surface.
    QSignalSpy reload(
        &applicationBindings,
        &GhosttyApplicationKeybindings::applicationActionRequested);
    QKeyEvent allReload(QEvent::KeyPress, Qt::Key_R,
                        Qt::ControlModifier, QString(QChar(0x12)));
    QCoreApplication::sendEvent(activePane, &allReload);
    QKeyEvent allReloadRelease(QEvent::KeyRelease, Qt::Key_R,
                               Qt::ControlModifier);
    QCoreApplication::sendEvent(activePane, &allReloadRelease);
    QCOMPARE(reload.count(), 1);
    QCOMPARE(qvariant_cast<ApplicationAction>(
                 reload.constFirst().constFirst()),
             ApplicationAction::ReloadConfig);
    QCOMPARE(forwarded.count(), 0);

    // Broad bindings ignore performable/unconsumed and remain consumed even
    // when no target has the dynamic state required to execute the action.
    // Each worker is still asked: its correlated result, rather than the
    // GUI's eventually-consistent selection cache, decides performability.
    QKeyEvent unavailableCopy(QEvent::KeyPress, Qt::Key_P,
                              Qt::ControlModifier, QString(QChar(0x10)));
    QCoreApplication::sendEvent(activePane, &unavailableCopy);
    QKeyEvent unavailableCopyRelease(QEvent::KeyRelease, Qt::Key_P,
                                     Qt::ControlModifier);
    QCoreApplication::sendEvent(activePane, &unavailableCopyRelease);
    QCOMPARE(activeCopy.count(), 1);
    QCOMPARE(inactiveCopy.count(), 1);
    QVERIFY(activeCopy.constFirst().constFirst().toULongLong() != 0);
    QVERIFY(inactiveCopy.constFirst().constFirst().toULongLong() != 0);
    QCOMPARE(forwarded.count(), 0);

    // Application-scoped broad actions execute once, not once per pane.
    applicationBindings.dispatchBroadActions(
        {QStringLiteral("new_window")});
    QTRY_COMPARE_WITH_TIMEOUT(reload.count(), 2, 1000);
    QCOMPARE(qvariant_cast<ApplicationAction>(
                 reload.constLast().constFirst()),
             ApplicationAction::NewWindow);
    QVERIFY(!workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("new_window")));
    QCOMPARE(reload.count(), 2);

    applicationBindings.dispatchBroadActions(
        {QStringLiteral("open_config")});
    QTRY_COMPARE_WITH_TIMEOUT(reload.count(), 3, 1000);
    QCOMPARE(qvariant_cast<ApplicationAction>(
                 reload.constLast().constFirst()),
             ApplicationAction::OpenConfig);
    QVERIFY(!workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("open_config")));
    QCOMPARE(reload.count(), 3);

    // Other/right remain ordinary surface fanout, not the special broad
    // close-every-surface path. The first stable source keeps its own tab.
    const TabId firstTabId = workspace.tabModel()->idAt(0);
    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    applicationBindings.dispatchBroadActions(
        {QStringLiteral("close_tab:other")});
    QCOMPARE(quit.count(), 0);
    QCOMPARE(tabIds(workspace), QVector<TabId>({firstTabId}));
}

void TerminalWorkspaceTest::broadPasteActionsReachEveryPane()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.clipboardPaste.protection = false;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    workspace.splitRight();
    workspace.newTab();
    const QList<TerminalPane *> panes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 3);

    std::vector<std::unique_ptr<QSignalSpy>> pasted;
    pasted.reserve(static_cast<size_t>(panes.size()));
    for (TerminalPane *pane : panes) {
        TerminalController *const controller =
            pane->findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        pasted.push_back(std::make_unique<QSignalSpy>(
            controller, &TerminalController::pasteRequested));
    }

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    GhosttyApplicationKeybindings bindings(options, false);
    bindings.registerWorkspace(&workspace);

    clipboard->setText(QStringLiteral("standard broad paste"),
                       QClipboard::Clipboard);
    bindings.dispatchBroadActions(
        {QStringLiteral("paste_from_clipboard")});
    for (const auto &spy : pasted) {
        QCOMPARE(spy->count(), 1);
        QCOMPARE(spy->constLast().constFirst().toString(),
                 QStringLiteral("standard broad paste"));
    }

    clipboard->setText(QStringLiteral("standard is not primary"),
                       QClipboard::Clipboard);
    if (clipboard->supportsSelection()) {
        clipboard->setText(QStringLiteral("primary broad paste"),
                           QClipboard::Selection);
    }
    bindings.dispatchBroadActions(
        {QStringLiteral("paste_from_selection")});
    for (const auto &spy : pasted) {
        const int expectedCount = clipboard->supportsSelection() ? 2 : 1;
        QCOMPARE(spy->count(), expectedCount);
        if (clipboard->supportsSelection()) {
            QCOMPARE(spy->constLast().constFirst().toString(),
                     QStringLiteral("primary broad paste"));
        }
    }

    clipboard->clear(QClipboard::Clipboard);
    if (clipboard->supportsSelection()) {
        clipboard->clear(QClipboard::Selection);
    }
}

void TerminalWorkspaceTest::
    broadWriteScreenOpenCreatesDistinctPerPaneArtifacts()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {
        QStringLiteral("/bin/sleep"),
        QStringLiteral("30"),
    };
    // Derived panes drop the one-shot program and hold. Give them a persistent
    // ordinary command so every screen export completes in one live action
    // epoch now that pidfd reports /bin/true exits without polling latency.
    options.ordinaryCommand = TerminalCommand::direct({
        QByteArrayLiteral("/bin/sleep"),
        QByteArrayLiteral("30"),
    });
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QVector<QList<QUrl>> openedPerPane;
    QSet<QString> artifactDirectories;
    const auto cleanupArtifacts = qScopeGuard([&artifactDirectories] {
        for (const QString &directory : artifactDirectories) {
            static_cast<void>(QDir(directory).removeRecursively());
        }
    });

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    workspace.splitRight();
    workspace.newTab();
    const QList<TerminalPane *> panes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 3);

    openedPerPane.resize(panes.size());
    for (qsizetype index = 0; index < panes.size(); ++index) {
        panes.at(index)->setUrlOpener(
            [&, index](const QUrl &url) {
                openedPerPane[index].append(url);
                if (url.isLocalFile()) {
                    artifactDirectories.insert(
                        QFileInfo(url.toLocalFile()).absolutePath());
                }
                return true;
            });
    }

    GhosttyApplicationKeybindings bindings(options, false);
    bindings.registerWorkspace(&workspace);
    bindings.dispatchBroadActions(
        {QStringLiteral("write_screen_file:open")});

    const auto everyPaneOpenedOnce = [&openedPerPane] {
        return std::ranges::all_of(
            openedPerPane,
            [](const QList<QUrl> &urls) { return urls.size() == 1; });
    };
    QTRY_VERIFY_WITH_TIMEOUT(everyPaneOpenedOnce(), 5000);

    QSet<QString> artifactPaths;
    for (const QList<QUrl> &opened : openedPerPane) {
        const QUrl url = opened.constFirst();
        QVERIFY(url.isLocalFile());
        const QString path = url.toLocalFile();
        const QFileInfo artifact(path);
        QVERIFY(artifact.isAbsolute());
        QVERIFY(artifact.isFile());
        QCOMPARE(artifact.fileName(), QStringLiteral("screen.txt"));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        static_cast<void>(file.readAll());
        QCOMPARE(file.error(), QFileDevice::NoError);
        artifactPaths.insert(artifact.canonicalFilePath());
    }

    QCOMPARE(artifactPaths.size(), panes.size());
    QCOMPARE(artifactDirectories.size(), panes.size());
}

void TerminalWorkspaceTest::
    broadTerminalBarrierOrdersCommitsAndDefersInputs()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    GhosttyKeybindConfig config;
    config.root = {GhosttyKeybindDefinition{
        .sequence = {GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = 'g',
            .modifiers = GhosttyKeybindCtrl,
        }},
        .actions = {QStringLiteral("increase_font_size:4")},
        .flags = GhosttyKeybindFlags{.global = true},
    }};
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));
    TerminalWorkspace::setDefaultLaunchOptions(options);

    constexpr qsizetype targetCount = 3;
    std::array<std::unique_ptr<TerminalWorkspace>, targetCount> workspaces;
    QVector<TerminalPane *> panes;
    QVector<TerminalController *> controllers;
    QVector<quint64> requestIds(targetCount, 0);
    QVector<qreal> initialFontSizes;
    QVector<QVector<qreal>> fontSizes(targetCount);
    QVector<qsizetype> openOrder;
    QVector<QString> openedPaths(targetCount);

    GhosttyApplicationKeybindings bindings(options, false);
    for (qsizetype index = 0; index < targetCount; ++index) {
        workspaces[static_cast<std::size_t>(index)] =
            std::make_unique<TerminalWorkspace>();
        TerminalWorkspace *const workspace =
            workspaces[static_cast<std::size_t>(index)].get();
        QVERIFY(workspace->initialize(
            options, TerminalSessionStartMode::Deferred));
        QCOMPARE(workspace->tabCount(), 1);
        bindings.registerWorkspace(workspace);

        TerminalPane *const pane =
            workspace->findChild<TerminalPane *>();
        QVERIFY(pane != nullptr);
        TerminalController *const controller =
            pane->findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        panes.append(pane);
        controllers.append(controller);
        initialFontSizes.append(pane->fontPointSize());

        pane->setUrlOpener(
            [&, index](const QUrl &url) {
                openedPaths[index] = url.toLocalFile();
                openOrder.append(index);
                return true;
            });
        connect(
            pane, &TerminalPane::fontPointSizeChanged, this,
            [&, index] {
                fontSizes[index].append(
                    panes.at(index)->fontPointSize());
            });
        connect(
            controller,
            &TerminalController::writeTerminalFileRequested,
            this,
            [&, index](
                quint64 requestId,
                const TerminalWriteFileAction &) {
                QCOMPARE(requestIds.at(index), quint64(0));
                requestIds[index] = requestId;
            });
    }

    bindings.dispatchBroadActions({
        QStringLiteral("write_screen_file:open"),
        QStringLiteral("increase_font_size:1"),
    });

    // Every target begins the worker-owned action before the coordinator
    // waits. No opener or later action may run while one result is missing.
    QVERIFY(std::ranges::all_of(
        requestIds, [](quint64 requestId) { return requestId != 0; }));
    QVERIFY(openOrder.isEmpty());
    for (qsizetype index = 0; index < targetCount; ++index) {
        QCOMPARE(panes.at(index)->fontPointSize(),
                 initialFontSizes.at(index));
        QVERIFY(fontSizes.at(index).isEmpty());
    }

    // Both a later external broad request and a root global key event join
    // the same FIFO behind the active barrier.
    bindings.dispatchBroadActions({
        QStringLiteral("increase_font_size:2"),
    });
    QKeyEvent keyPress(
        QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier,
        QString(QChar(0x07)));
    QCoreApplication::sendEvent(panes.constFirst(), &keyPress);
    QKeyEvent keyRelease(
        QEvent::KeyRelease, Qt::Key_G, Qt::ControlModifier);
    QCoreApplication::sendEvent(panes.constFirst(), &keyRelease);
    for (qsizetype index = 0; index < targetCount; ++index) {
        QCOMPARE(panes.at(index)->fontPointSize(),
                 initialFontSizes.at(index));
    }

    // Resolve the targets in reverse registration order. Partial completion
    // must remain invisible; the final result publishes every effect in the
    // stable workspace snapshot order.
    for (qsizetype index = targetCount - 1; index > 0; --index) {
        Q_EMIT controllers.at(index)->terminalActionReady(
            successfulOpenFileResult(
                requestIds.at(index),
                QStringLiteral("/tmp/ghostty-qt-barrier-%1.txt")
                    .arg(index)));
        QCoreApplication::processEvents();
        QVERIFY(openOrder.isEmpty());
        for (qsizetype fontIndex = 0;
             fontIndex < targetCount; ++fontIndex) {
            QCOMPARE(
                panes.at(fontIndex)->fontPointSize(),
                initialFontSizes.at(fontIndex));
        }
    }
    Q_EMIT controllers.constFirst()->terminalActionReady(
        successfulOpenFileResult(
            requestIds.constFirst(),
            QStringLiteral("/tmp/ghostty-qt-barrier-0.txt")));
    QCoreApplication::processEvents();

    QCOMPARE(openOrder, QVector<qsizetype>({0, 1, 2}));
    for (qsizetype index = 0; index < targetCount; ++index) {
        const qreal initial = initialFontSizes.at(index);
        QCOMPARE(
            openedPaths.at(index),
            QStringLiteral("/tmp/ghostty-qt-barrier-%1.txt")
                .arg(index));
        QCOMPARE(
            fontSizes.at(index),
            QVector<qreal>({initial + 1.0,
                            initial + 3.0,
                            initial + 7.0}));
        QCOMPARE(panes.at(index)->fontPointSize(), initial + 7.0);
    }
}

void TerminalWorkspaceTest::broadSelectionBarriersOrderEffects()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    constexpr qsizetype targetCount = 2;
    std::array<std::unique_ptr<TerminalWorkspace>, targetCount> workspaces;
    std::array<TerminalPane *, targetCount> panes{};
    std::array<TerminalController *, targetCount> controllers{};
    std::array<quint64, targetCount> adjustmentIds{};
    std::array<quint64, targetCount> scrollIds{};
    std::array<quint64, targetCount> searchIds{};
    std::array<quint64, targetCount> copyIds{};
    std::array<qreal, targetCount> initialFontSizes{};
    QStringList requestOrder;
    QStringList effectOrder;

    GhosttyApplicationKeybindings bindings(options, false);
    for (qsizetype index = 0; index < targetCount; ++index) {
        const std::size_t slot = static_cast<std::size_t>(index);
        workspaces[slot] = std::make_unique<TerminalWorkspace>();
        QVERIFY(workspaces[slot]->initialize(
            options, TerminalSessionStartMode::Deferred));
        bindings.registerWorkspace(workspaces[slot].get());

        panes[slot] =
            workspaces[slot]->findChild<TerminalPane *>();
        QVERIFY(panes[slot] != nullptr);
        controllers[slot] =
            panes[slot]->findChild<TerminalController *>();
        QVERIFY(controllers[slot] != nullptr);
        initialFontSizes[slot] = panes[slot]->fontPointSize();

        connect(
            controllers[slot],
            &TerminalController::selectionAdjustmentActionRequested,
            this,
            [&, index, slot](
                quint64 requestId,
                TerminalSelectionAdjustment adjustment) {
                QCOMPARE(adjustmentIds[slot], quint64(0));
                QCOMPARE(adjustment,
                         TerminalSelectionAdjustment::Right);
                adjustmentIds[slot] = requestId;
                requestOrder.append(
                    QStringLiteral("adjust:%1").arg(index));
            });
        connect(
            controllers[slot],
            &TerminalController::scrollToSelectionActionRequested,
            this,
            [&, index, slot](quint64 requestId) {
                QCOMPARE(scrollIds[slot], quint64(0));
                scrollIds[slot] = requestId;
                requestOrder.append(
                    QStringLiteral("scroll:%1").arg(index));
            });
        connect(
            controllers[slot],
            &TerminalController::searchSelectionActionRequested,
            this,
            [&, index, slot](quint64 requestId) {
                QCOMPARE(searchIds[slot], quint64(0));
                searchIds[slot] = requestId;
                requestOrder.append(
                    QStringLiteral("search:%1").arg(index));
            });
        connect(
            controllers[slot],
            &TerminalController::copyActionRequested,
            this,
            [&, index, slot](quint64 requestId) {
                QCOMPARE(copyIds[slot], quint64(0));
                copyIds[slot] = requestId;
                requestOrder.append(
                    QStringLiteral("copy:%1").arg(index));
                effectOrder.append(
                    QStringLiteral("copy-request:%1").arg(index));
            });
        connect(
            panes[slot], &TerminalPane::searchUiFocusRequested,
            this, [&, index] {
                effectOrder.append(
                    QStringLiteral("search-effect:%1").arg(index));
            });
    }

    bindings.dispatchBroadActions({
        QStringLiteral("adjust_selection:right"),
        QStringLiteral("scroll_to_selection"),
        QStringLiteral("search_selection"),
        QStringLiteral("copy_to_clipboard"),
        QStringLiteral("increase_font_size:1"),
    });

    QCOMPARE(
        requestOrder,
        QStringList({QStringLiteral("adjust:0"),
                     QStringLiteral("adjust:1")}));
    QVERIFY(std::ranges::all_of(
        adjustmentIds,
        [](quint64 requestId) { return requestId != 0; }));
    QVERIFY(std::ranges::all_of(
        scrollIds, [](quint64 requestId) { return requestId == 0; }));

    Q_EMIT controllers[1]->terminalActionReady(
        successfulTerminalActionResult(adjustmentIds[1]));
    QCoreApplication::sendPostedEvents(panes[1], QEvent::MetaCall);
    QCOMPARE(requestOrder.size(), 2);
    Q_EMIT controllers[0]->terminalActionReady(
        successfulTerminalActionResult(adjustmentIds[0]));
    QCoreApplication::sendPostedEvents(panes[0], QEvent::MetaCall);
    QCOMPARE(
        requestOrder,
        QStringList({QStringLiteral("adjust:0"),
                     QStringLiteral("adjust:1"),
                     QStringLiteral("scroll:0"),
                     QStringLiteral("scroll:1")}));

    Q_EMIT controllers[1]->terminalActionReady(
        successfulTerminalActionResult(scrollIds[1]));
    QCoreApplication::sendPostedEvents(panes[1], QEvent::MetaCall);
    QCOMPARE(requestOrder.size(), 4);
    Q_EMIT controllers[0]->terminalActionReady(
        successfulTerminalActionResult(scrollIds[0]));
    QCoreApplication::sendPostedEvents(panes[0], QEvent::MetaCall);
    QCOMPARE(
        requestOrder,
        QStringList({QStringLiteral("adjust:0"),
                     QStringLiteral("adjust:1"),
                     QStringLiteral("scroll:0"),
                     QStringLiteral("scroll:1"),
                     QStringLiteral("search:0"),
                     QStringLiteral("search:1")}));

    Q_EMIT controllers[1]->terminalActionReady(
        successfulTerminalActionResult(
            searchIds[1], TerminalActionEffect::StartSearch,
            QStringLiteral("selected one")));
    QCoreApplication::sendPostedEvents(panes[1], QEvent::MetaCall);
    QVERIFY(effectOrder.isEmpty());
    QCOMPARE(requestOrder.size(), 6);
    Q_EMIT controllers[0]->terminalActionReady(
        successfulTerminalActionResult(
            searchIds[0], TerminalActionEffect::StartSearch,
            QStringLiteral("selected zero")));
    QCoreApplication::sendPostedEvents(panes[0], QEvent::MetaCall);

    QCOMPARE(
        effectOrder,
        QStringList({QStringLiteral("search-effect:0"),
                     QStringLiteral("search-effect:1"),
                     QStringLiteral("copy-request:0"),
                     QStringLiteral("copy-request:1")}));
    QCOMPARE(
        requestOrder,
        QStringList({QStringLiteral("adjust:0"),
                     QStringLiteral("adjust:1"),
                     QStringLiteral("scroll:0"),
                     QStringLiteral("scroll:1"),
                     QStringLiteral("search:0"),
                     QStringLiteral("search:1"),
                     QStringLiteral("copy:0"),
                     QStringLiteral("copy:1")}));
    QVERIFY(panes[0]->searchUiActive());
    QVERIFY(panes[1]->searchUiActive());
    QCOMPARE(panes[0]->searchUiText(),
             QStringLiteral("selected zero"));
    QCOMPARE(panes[1]->searchUiText(),
             QStringLiteral("selected one"));
    for (qsizetype index = 0; index < targetCount; ++index) {
        const std::size_t slot = static_cast<std::size_t>(index);
        QCOMPARE(panes[slot]->fontPointSize(),
                 initialFontSizes[slot]);
    }

    Q_EMIT controllers[1]->terminalActionReady(
        successfulTerminalActionResult(copyIds[1]));
    QCoreApplication::sendPostedEvents(panes[1], QEvent::MetaCall);
    for (qsizetype index = 0; index < targetCount; ++index) {
        const std::size_t slot = static_cast<std::size_t>(index);
        QCOMPARE(panes[slot]->fontPointSize(),
                 initialFontSizes[slot]);
    }
    Q_EMIT controllers[0]->terminalActionReady(
        successfulTerminalActionResult(copyIds[0]));
    QCoreApplication::sendPostedEvents(panes[0], QEvent::MetaCall);
    for (qsizetype index = 0; index < targetCount; ++index) {
        const std::size_t slot = static_cast<std::size_t>(index);
        QCOMPARE(panes[slot]->fontPointSize(),
                 initialFontSizes[slot] + 1.0);
    }
}

void TerminalWorkspaceTest::
    broadRetainedSearchResultExpiresOnSessionExit()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    constexpr qsizetype targetCount = 2;
    std::array<std::unique_ptr<TerminalWorkspace>, targetCount> workspaces;
    std::array<TerminalPane *, targetCount> panes{};
    std::array<TerminalController *, targetCount> controllers{};
    std::array<quint64, targetCount> requestIds{};

    GhosttyApplicationKeybindings bindings(options, false);
    for (qsizetype index = 0; index < targetCount; ++index) {
        const std::size_t slot = static_cast<std::size_t>(index);
        workspaces[slot] = std::make_unique<TerminalWorkspace>();
        QVERIFY(workspaces[slot]->initialize(
            options, TerminalSessionStartMode::Deferred));
        bindings.registerWorkspace(workspaces[slot].get());

        panes[slot] =
            workspaces[slot]->findChild<TerminalPane *>();
        QVERIFY(panes[slot] != nullptr);
        controllers[slot] =
            panes[slot]->findChild<TerminalController *>();
        QVERIFY(controllers[slot] != nullptr);
        connect(
            controllers[slot],
            &TerminalController::searchSelectionActionRequested,
            this,
            [&, slot](quint64 requestId) {
                requestIds[slot] = requestId;
            });
    }

    QSignalSpy staleActive(
        panes[0], &TerminalPane::searchUiActiveChanged);
    QSignalSpy staleText(
        panes[0], &TerminalPane::searchUiTextChanged);
    QSignalSpy staleFocus(
        panes[0], &TerminalPane::searchUiFocusRequested);
    QSignalSpy staleSearch(
        controllers[0], &TerminalController::searchRequested);
    QSignalSpy survivorActive(
        panes[1], &TerminalPane::searchUiActiveChanged);
    QSignalSpy survivorText(
        panes[1], &TerminalPane::searchUiTextChanged);
    QSignalSpy survivorFocus(
        panes[1], &TerminalPane::searchUiFocusRequested);
    QSignalSpy applicationActions(
        &bindings,
        &GhosttyApplicationKeybindings::applicationActionRequested);

    bindings.dispatchBroadActions({
        QStringLiteral("search_selection"),
        QStringLiteral("reload_config"),
    });
    QVERIFY(requestIds[0] != 0);
    QVERIFY(requestIds[1] != 0);

    // The pane consumes this completion and hands the stamped result to the
    // process barrier. Publication remains blocked on target one.
    Q_EMIT controllers[0]->terminalActionReady(
        successfulTerminalActionResult(
            requestIds[0], TerminalActionEffect::StartSearch,
            QStringLiteral("expired selection")));
    QCoreApplication::sendPostedEvents(panes[0], QEvent::MetaCall);
    QVERIFY(!panes[0]->searchUiActive());
    QCOMPARE(staleActive.count(), 0);
    QCOMPARE(staleText.count(), 0);
    QCOMPARE(staleFocus.count(), 0);
    QCOMPARE(staleSearch.count(), 0);
    QCOMPARE(applicationActions.count(), 0);

    // A held surface survives its child exit, but crossing that lifecycle
    // boundary expires the result already retained by BroadExecution.
    Q_EMIT controllers[0]->sessionExited(0, 0, true, false, 500, false);
    QVERIFY(!panes[0]->searchUiActive());
    QCOMPARE(staleActive.count(), 0);
    QCOMPARE(staleText.count(), 0);
    QCOMPARE(staleFocus.count(), 0);
    QCOMPARE(staleSearch.count(), 0);
    QCOMPARE(applicationActions.count(), 0);

    Q_EMIT controllers[1]->terminalActionReady(
        successfulTerminalActionResult(
            requestIds[1], TerminalActionEffect::StartSearch,
            QStringLiteral("current selection")));
    QCoreApplication::sendPostedEvents(panes[1], QEvent::MetaCall);

    // Target zero's stale effect is rejected at publication. The live target
    // commits once, and the following application action proves the barrier
    // and chain completed exactly once.
    QVERIFY(!panes[0]->searchUiActive());
    QVERIFY(panes[0]->searchUiText().isEmpty());
    QCOMPARE(staleActive.count(), 0);
    QCOMPARE(staleText.count(), 0);
    QCOMPARE(staleFocus.count(), 0);
    QCOMPARE(staleSearch.count(), 0);
    QVERIFY(panes[1]->searchUiActive());
    QCOMPARE(panes[1]->searchUiText(),
             QStringLiteral("current selection"));
    QCOMPARE(survivorActive.count(), 1);
    QCOMPARE(survivorText.count(), 1);
    QCOMPARE(survivorFocus.count(), 1);
    QCOMPARE(applicationActions.count(), 1);
    QCOMPARE(
        qvariant_cast<ApplicationAction>(
            applicationActions.constFirst().constFirst()),
        ApplicationAction::ReloadConfig);

    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents();
    QCOMPARE(staleActive.count(), 0);
    QCOMPARE(staleText.count(), 0);
    QCOMPARE(staleFocus.count(), 0);
    QCOMPARE(staleSearch.count(), 0);
    QCOMPARE(survivorActive.count(), 1);
    QCOMPARE(survivorText.count(), 1);
    QCOMPARE(survivorFocus.count(), 1);
    QCOMPARE(applicationActions.count(), 1);
}

void TerminalWorkspaceTest::
    searchEffectDestructionDefersBroadContinuation()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    constexpr qsizetype targetCount = 2;
    std::array<std::unique_ptr<TerminalWorkspace>, targetCount> workspaces;
    std::array<QPointer<TerminalPane>, targetCount> panes{};
    std::array<QPointer<TerminalController>, targetCount> controllers{};
    std::array<quint64, targetCount> searchIds{};
    std::array<quint64, targetCount> copyIds{};
    int firstSearchEffects = 0;
    int survivorSearchEffects = 0;
    int firstCopyRequests = 0;
    int survivorCopyRequests = 0;
    bool deletingFirstWorkspace = false;
    bool survivorEffectDuringDestruction = false;
    bool copyStartedDuringDestruction = false;

    GhosttyApplicationKeybindings bindings(options, false);
    for (qsizetype index = 0; index < targetCount; ++index) {
        const std::size_t slot = static_cast<std::size_t>(index);
        workspaces[slot] = std::make_unique<TerminalWorkspace>();
        QVERIFY(workspaces[slot]->initialize(
            options, TerminalSessionStartMode::Deferred));
        bindings.registerWorkspace(workspaces[slot].get());
        panes[slot] =
            workspaces[slot]->findChild<TerminalPane *>();
        QVERIFY(panes[slot] != nullptr);
        controllers[slot] =
            panes[slot]->findChild<TerminalController *>();
        QVERIFY(controllers[slot] != nullptr);
        connect(
            controllers[slot],
            &TerminalController::searchSelectionActionRequested,
            this,
            [&, slot](quint64 requestId) {
                searchIds[slot] = requestId;
            });
    }

    connect(
        panes[0], &TerminalPane::searchUiActiveChanged,
        this, [&] {
            ++firstSearchEffects;
            deletingFirstWorkspace = true;
            workspaces[0].reset();
            deletingFirstWorkspace = false;
        });
    connect(
        panes[1], &TerminalPane::searchUiFocusRequested,
        this, [&] {
            ++survivorSearchEffects;
            survivorEffectDuringDestruction |=
                deletingFirstWorkspace;
        });
    connect(
        controllers[0], &TerminalController::copyActionRequested,
        this, [&](quint64 requestId) {
            ++firstCopyRequests;
            copyIds[0] = requestId;
            copyStartedDuringDestruction |=
                deletingFirstWorkspace;
        });
    connect(
        controllers[1], &TerminalController::copyActionRequested,
        this, [&](quint64 requestId) {
            ++survivorCopyRequests;
            copyIds[1] = requestId;
            copyStartedDuringDestruction |=
                deletingFirstWorkspace;
        });
    QSignalSpy applicationActions(
        &bindings,
        &GhosttyApplicationKeybindings::applicationActionRequested);

    bindings.dispatchBroadActions({
        QStringLiteral("search_selection"),
        QStringLiteral("copy_to_clipboard"),
        QStringLiteral("reload_config"),
    });
    QVERIFY(searchIds[0] != 0);
    QVERIFY(searchIds[1] != 0);

    // Prepare the first snapshot entry without completing the barrier. The
    // survivor emits the final result so deleting target zero during its
    // StartSearch publication cannot invalidate the signal sender.
    Q_EMIT controllers[0]->terminalActionReady(
        successfulTerminalActionResult(
            searchIds[0], TerminalActionEffect::StartSearch,
            QStringLiteral("destroy me")));
    QCoreApplication::sendPostedEvents(panes[0], QEvent::MetaCall);
    QCOMPARE(firstSearchEffects, 0);
    QCOMPARE(survivorSearchEffects, 0);

    Q_EMIT controllers[1]->terminalActionReady(
        successfulTerminalActionResult(
            searchIds[1], TerminalActionEffect::StartSearch,
            QStringLiteral("survivor")));
    QCoreApplication::sendPostedEvents(panes[1], QEvent::MetaCall);

    QVERIFY(workspaces[0] == nullptr);
    QVERIFY(panes[0] == nullptr);
    QVERIFY(controllers[0] == nullptr);
    QCOMPARE(firstSearchEffects, 1);
    QCOMPARE(survivorSearchEffects, 0);
    QCOMPARE(firstCopyRequests, 0);
    QCOMPARE(survivorCopyRequests, 0);
    QCOMPARE(applicationActions.count(), 0);
    QVERIFY(!survivorEffectDuringDestruction);
    QVERIFY(!copyStartedDuringDestruction);

    // Destruction observed while committing target zero pauses the commit
    // cursor. A later event turn resumes at target one exactly once, then
    // takes a fresh snapshot for the copy entry.
    QCoreApplication::processEvents();
    QCOMPARE(firstSearchEffects, 1);
    QCOMPARE(survivorSearchEffects, 1);
    QCOMPARE(firstCopyRequests, 0);
    QCOMPARE(survivorCopyRequests, 1);
    QVERIFY(copyIds[1] != 0);
    QCOMPARE(applicationActions.count(), 0);
    QVERIFY(!survivorEffectDuringDestruction);
    QVERIFY(!copyStartedDuringDestruction);
    QVERIFY(panes[1]->searchUiActive());
    QCOMPARE(panes[1]->searchUiText(),
             QStringLiteral("survivor"));

    Q_EMIT controllers[1]->terminalActionReady(
        successfulTerminalActionResult(copyIds[1]));
    QCoreApplication::sendPostedEvents(panes[1], QEvent::MetaCall);
    QCOMPARE(applicationActions.count(), 1);
    QCOMPARE(
        qvariant_cast<ApplicationAction>(
            applicationActions.constFirst().constFirst()),
        ApplicationAction::ReloadConfig);
    QCOMPARE(firstSearchEffects, 1);
    QCOMPARE(survivorSearchEffects, 1);
}

void TerminalWorkspaceTest::
    broadDeferredInputFifoSurvivesReplayReentrancy()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QVERIFY(workspace.initialize(
        options, TerminalSessionStartMode::Deferred));
    TerminalPane *const pane =
        workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *const controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    pane->setUrlOpener([](const QUrl &) { return true; });

    GhosttyApplicationKeybindings bindings(options, false);
    bindings.registerWorkspace(&workspace);
    quint64 requestId = 0;
    connect(
        controller,
        &TerminalController::writeTerminalFileRequested,
        this,
        [&requestId](
            quint64 emittedRequestId,
            const TerminalWriteFileAction &) {
            requestId = emittedRequestId;
        });

    QStringList order;
    bool injected = false;
    connect(
        controller, &TerminalController::keyRequested, pane,
        [&](const TerminalKeyInput &input) {
            order.append(QStringLiteral("%1-%2")
                             .arg(QChar(input.key))
                             .arg(input.pressed
                                      ? QStringLiteral("press")
                                      : QStringLiteral("release")));
            if (injected || input.key != Qt::Key_C
                || !input.pressed) {
                return;
            }
            injected = true;
            QKeyEvent nested(
                QEvent::KeyPress, Qt::Key_E, Qt::NoModifier,
                QStringLiteral("e"));
            QCoreApplication::sendEvent(pane, &nested);
            QInputMethodEvent nestedInput;
            nestedInput.setCommitString(
                QStringLiteral("new-ime"));
            QCoreApplication::sendEvent(pane, &nestedInput);
            bindings.dispatchBroadActions(
                {QStringLiteral("increase_font_size:1")});
        });
    connect(
        controller, &TerminalController::inputMethodRequested, pane,
        [&order](const TerminalInputMethodInput &input) {
            order.append(
                QStringLiteral("ime:%1").arg(input.commitText));
        });
    connect(
        pane, &TerminalPane::fontPointSizeChanged, pane,
        [&order] { order.append(QStringLiteral("font")); });

    bindings.dispatchBroadActions(
        {QStringLiteral("write_screen_file:open")});
    QVERIFY(requestId != 0);

    QKeyEvent cPress(
        QEvent::KeyPress, Qt::Key_C, Qt::NoModifier,
        QStringLiteral("c"));
    QCoreApplication::sendEvent(pane, &cPress);
    QInputMethodEvent oldInput;
    oldInput.setCommitString(QStringLiteral("old-ime"));
    QCoreApplication::sendEvent(pane, &oldInput);
    QKeyEvent cRelease(
        QEvent::KeyRelease, Qt::Key_C, Qt::NoModifier);
    QCoreApplication::sendEvent(pane, &cRelease);
    QKeyEvent dPress(
        QEvent::KeyPress, Qt::Key_D, Qt::NoModifier,
        QStringLiteral("d"));
    QCoreApplication::sendEvent(pane, &dPress);
    QVERIFY(order.isEmpty());

    Q_EMIT controller->terminalActionReady(
        successfulOpenFileResult(
            requestId,
            QStringLiteral("/tmp/global-reentrant-input-fifo.txt")));
    QCoreApplication::processEvents();

    QVERIFY(injected);
    QCOMPARE(
        order,
        QStringList({
            QStringLiteral("C-press"),
            QStringLiteral("ime:old-ime"),
            QStringLiteral("C-release"),
            QStringLiteral("D-press"),
            QStringLiteral("E-press"),
            QStringLiteral("ime:new-ime"),
            QStringLiteral("font"),
        }));
}

void TerminalWorkspaceTest::
    broadTerminalBarrierSkipsDestroyedTargets()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    constexpr qsizetype targetCount = 3;
    std::array<QPointer<TerminalWorkspace>, targetCount> workspaces;
    std::array<QPointer<TerminalPane>, targetCount> panes;
    std::array<QPointer<TerminalController>, targetCount> controllers;
    QVector<quint64> requestIds(targetCount, 0);
    QVector<qsizetype> openOrder;
    const auto cleanup = qScopeGuard([&workspaces] {
        for (QPointer<TerminalWorkspace> &workspace : workspaces) {
            delete workspace.data();
        }
    });

    GhosttyApplicationKeybindings bindings(options, false);
    for (qsizetype index = 0; index < targetCount; ++index) {
        workspaces[static_cast<std::size_t>(index)] =
            new TerminalWorkspace;
        TerminalWorkspace *const workspace =
            workspaces[static_cast<std::size_t>(index)].data();
        QVERIFY(workspace->initialize(
            options, TerminalSessionStartMode::Deferred));
        bindings.registerWorkspace(workspace);

        panes[static_cast<std::size_t>(index)] =
            workspace->findChild<TerminalPane *>();
        QVERIFY(panes[static_cast<std::size_t>(index)] != nullptr);
        controllers[static_cast<std::size_t>(index)] =
            panes[static_cast<std::size_t>(index)]
                ->findChild<TerminalController *>();
        QVERIFY(controllers[static_cast<std::size_t>(index)]
                != nullptr);

        panes[static_cast<std::size_t>(index)]->setUrlOpener(
            [&, index](const QUrl &) {
                openOrder.append(index);
                return true;
            });
        connect(
            controllers[static_cast<std::size_t>(index)],
            &TerminalController::writeTerminalFileRequested,
            this,
            [&, index](
                quint64 requestId,
                const TerminalWriteFileAction &) {
                requestIds[index] = requestId;
            });
    }

    QSignalSpy applicationActions(
        &bindings,
        &GhosttyApplicationKeybindings::applicationActionRequested);
    bool resumedInsideEarlierDestroyedObserver = false;
    connect(
        workspaces[1], &QObject::destroyed, this,
        [&] {
            // This observer predates the broad snapshot's destruction
            // connection. A nested event turn can therefore deliver the
            // remaining results before the coordinator's destroyed slot.
            Q_EMIT controllers[2]->terminalActionReady(
                successfulOpenFileResult(
                    requestIds.at(2),
                    QStringLiteral(
                        "/tmp/ghostty-qt-survivor-2.txt")));
            Q_EMIT controllers[0]->terminalActionReady(
                successfulOpenFileResult(
                    requestIds.at(0),
                    QStringLiteral(
                        "/tmp/ghostty-qt-survivor-0.txt")));
            QCoreApplication::processEvents();
            resumedInsideEarlierDestroyedObserver =
                !openOrder.isEmpty()
                || applicationActions.count() != 0;
        });
    bindings.dispatchBroadActions({
        QStringLiteral("write_screen_file:open"),
        QStringLiteral("reload_config"),
    });
    QVERIFY(std::ranges::all_of(
        requestIds, [](quint64 requestId) { return requestId != 0; }));
    QCOMPARE(applicationActions.count(), 0);

    // A prepared result remains buffered until the whole barrier resolves.
    // Destroying that target afterward must retain destruction observation,
    // skip its effect, and keep waiting for both live targets.
    Q_EMIT controllers[1]->terminalActionReady(
        successfulOpenFileResult(
            requestIds.at(1),
            QStringLiteral("/tmp/ghostty-qt-dead-target.txt")));
    QCoreApplication::processEvents();
    QVERIFY(openOrder.isEmpty());

    delete workspaces[1].data();
    QVERIFY(workspaces[1].isNull());
    QVERIFY(panes[1].isNull());
    QVERIFY(controllers[1].isNull());
    QVERIFY(!resumedInsideEarlierDestroyedObserver);
    QVERIFY(openOrder.isEmpty());
    QCOMPARE(applicationActions.count(), 0);
    QCoreApplication::processEvents();

    QTRY_COMPARE(
        openOrder, QVector<qsizetype>({0, 2}));
    QCOMPARE(applicationActions.count(), 1);
    QCOMPARE(qvariant_cast<ApplicationAction>(
                 applicationActions.constFirst().constFirst()),
             ApplicationAction::ReloadConfig);
}

void TerminalWorkspaceTest::
    finalBroadTargetDestructionDefersKeyReplay()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    GhosttyKeybindConfig config;
    config.root = {GhosttyKeybindDefinition{
        .sequence = {GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = 'g',
            .modifiers = GhosttyKeybindCtrl,
        }},
        .actions = {QStringLiteral("increase_font_size:1")},
        .flags = GhosttyKeybindFlags{.global = true},
    }};
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));
    TerminalWorkspace::setDefaultLaunchOptions(options);

    GhosttyApplicationKeybindings bindings(options, false);
    QPointer<TerminalWorkspace> barrierWorkspace =
        new TerminalWorkspace;
    QVERIFY(barrierWorkspace->initialize(
        options, TerminalSessionStartMode::Deferred));
    bindings.registerWorkspace(barrierWorkspace);

    TerminalController *const barrierController =
        barrierWorkspace->findChild<TerminalController *>();
    QVERIFY(barrierController != nullptr);

    quint64 requestId = 0;
    QPointer<TerminalWorkspace> survivor;
    TerminalPane *survivorPane = nullptr;
    qreal initialFontSize = 0.0;
    bool dispatchingBroadAction = false;
    bool deletingWorkspace = false;
    bool replayedBeforeDispatchReturned = false;
    bool replayedDuringDestruction = false;
    const auto cleanup = qScopeGuard([&survivor] {
        delete survivor.data();
    });
    connect(
        barrierController,
        &TerminalController::writeTerminalFileRequested,
        this,
        [&](
            quint64 emittedRequestId,
            const TerminalWriteFileAction &) {
            requestId = emittedRequestId;
            // This workspace is registered after the current entry's target
            // snapshot. Its key events join the active process FIFO.
            survivor = new TerminalWorkspace;
            QVERIFY(survivor->initialize(
                options, TerminalSessionStartMode::Deferred));
            bindings.registerWorkspace(survivor);
            survivorPane = survivor->findChild<TerminalPane *>();
            QVERIFY(survivorPane != nullptr);
            initialFontSize = survivorPane->fontPointSize();
            connect(
                survivorPane, &TerminalPane::fontPointSizeChanged,
                this, [&] {
                    replayedBeforeDispatchReturned |=
                        dispatchingBroadAction;
                    replayedDuringDestruction |= deletingWorkspace;
                });

            QKeyEvent keyPress(
                QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier,
                QString(QChar(0x07)));
            QCoreApplication::sendEvent(survivorPane, &keyPress);
            QKeyEvent keyRelease(
                QEvent::KeyRelease, Qt::Key_G,
                Qt::ControlModifier);
            QCoreApplication::sendEvent(survivorPane, &keyRelease);
            QCOMPARE(
                survivorPane->fontPointSize(), initialFontSize);

            // Resolve the sole target while request dispatch is still
            // unwinding. The continuation must remember that destruction
            // requires a later event turn even though target startup has not
            // finished yet.
            deletingWorkspace = true;
            delete barrierWorkspace.data();
            deletingWorkspace = false;
        });

    dispatchingBroadAction = true;
    bindings.dispatchBroadActions(
        {QStringLiteral("write_screen_file:open")});
    dispatchingBroadAction = false;

    QVERIFY(requestId != 0);
    QVERIFY(barrierWorkspace.isNull());
    QVERIFY(survivor != nullptr);
    QVERIFY(survivorPane != nullptr);
    QVERIFY(!replayedBeforeDispatchReturned);
    QVERIFY(!replayedDuringDestruction);
    QCOMPARE(survivorPane->fontPointSize(), initialFontSize);

    QCoreApplication::processEvents();
    QVERIFY(!replayedBeforeDispatchReturned);
    QVERIFY(!replayedDuringDestruction);
    QCOMPARE(survivorPane->fontPointSize(), initialFontSize + 1.0);
}

void TerminalWorkspaceTest::
    destroyedDeferredReleaseClearsGlobalConsumption()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    GhosttyKeybindConfig config;
    config.root = {GhosttyKeybindDefinition{
        .sequence = {GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = 'g',
            .modifiers = GhosttyKeybindCtrl,
        }},
        .actions = {QStringLiteral("write_screen_file:open")},
        .flags = GhosttyKeybindFlags{.global = true},
    }};
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));
    TerminalWorkspace::setDefaultLaunchOptions(options);

    GhosttyApplicationKeybindings bindings(options, false);
    QPointer<TerminalWorkspace> workspace = new TerminalWorkspace;
    QVERIFY(workspace->initialize(
        options, TerminalSessionStartMode::Deferred));
    bindings.registerWorkspace(workspace);
    TerminalPane *const pane = workspace->findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    TerminalController *const controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    quint64 requestId = 0;
    connect(
        controller,
        &TerminalController::writeTerminalFileRequested,
        this,
        [&requestId](
            quint64 emittedRequestId,
            const TerminalWriteFileAction &) {
            requestId = emittedRequestId;
        });

    // The global press is consumed and starts a terminal barrier. Its
    // matching release is then deferred with the originating pane target.
    QKeyEvent keyPress(
        QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier,
        QString(QChar(0x07)));
    QCoreApplication::sendEvent(pane, &keyPress);
    QVERIFY(requestId != 0);
    QKeyEvent deferredRelease(
        QEvent::KeyRelease, Qt::Key_G, Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &deferredRelease);

    delete workspace.data();
    QVERIFY(workspace.isNull());
    QCoreApplication::processEvents();

    // The dropped release must retire the consumed identity. Otherwise this
    // unrelated receiver's later release with the same logical key is eaten
    // by the process-level application filter.
    KeyReleaseReceiver receiver;
    QKeyEvent laterRelease(
        QEvent::KeyRelease, Qt::Key_G, Qt::ControlModifier);
    QCoreApplication::sendEvent(&receiver, &laterRelease);
    QCOMPARE(receiver.releaseCount(), 1);
}

void TerminalWorkspaceTest::broadViewportAndSelectionActionsReachEveryPane()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.mouseReporting = false;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId firstTab = workspace.tabModel()->idAt(0);
    const PaneId firstPane = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *const configuredSource =
        workspace.findChild<TerminalPane *>();
    QVERIFY(configuredSource != nullptr);
    TerminalController *const configuredSourceController =
        configuredSource->findChild<TerminalController *>();
    QVERIFY(configuredSourceController != nullptr);
    QVERIFY(!configuredSourceController->mouseReportingEnabled());

    // A runtime toggle is surface-local. Splits and tabs created afterward
    // consume the configured snapshot rather than inheriting that override.
    QVERIFY(configuredSource->executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(configuredSourceController->mouseReportingEnabled());
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {firstTab, firstPane, 0},
    }));
    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);

    const QList<TerminalPane *> panes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 3);

    std::vector<std::unique_ptr<QSignalSpy>> scrollSpies;
    std::vector<std::unique_ptr<QSignalSpy>> selectAllSpies;
    std::vector<std::unique_ptr<QSignalSpy>> adjustmentActionSpies;
    std::vector<std::unique_ptr<QSignalSpy>> scrollToSelectionSpies;
    std::vector<std::unique_ptr<QSignalSpy>> searchSelectionSpies;
    std::vector<std::unique_ptr<QSignalSpy>> csiSpies;
    std::vector<std::unique_ptr<QSignalSpy>> resetSpies;
    std::vector<TerminalController *> controllers;
    controllers.reserve(static_cast<std::size_t>(panes.size()));
    QStringList controlFanoutOrder;
    for (qsizetype paneIndex = 0; paneIndex < panes.size(); ++paneIndex) {
        TerminalPane *pane = panes.at(paneIndex);
        TerminalController *controller =
            pane->findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        controllers.push_back(controller);
        QVERIFY(!controller->selectionAvailable());
        scrollSpies.emplace_back(std::make_unique<QSignalSpy>(
            controller, &TerminalController::scrollRequested));
        selectAllSpies.emplace_back(std::make_unique<QSignalSpy>(
            controller, &TerminalController::selectAllActionRequested));
        adjustmentActionSpies.emplace_back(
            std::make_unique<QSignalSpy>(
                controller,
                &TerminalController::selectionAdjustmentActionRequested));
        scrollToSelectionSpies.emplace_back(
            std::make_unique<QSignalSpy>(
                controller,
                &TerminalController::scrollToSelectionActionRequested));
        searchSelectionSpies.emplace_back(
            std::make_unique<QSignalSpy>(
                controller,
                &TerminalController::searchSelectionActionRequested));
        csiSpies.emplace_back(std::make_unique<QSignalSpy>(
            controller, &TerminalController::csiRequested));
        resetSpies.emplace_back(std::make_unique<QSignalSpy>(
            controller, &TerminalController::resetTerminalRequested));
        connect(controller, &TerminalController::csiRequested,
                this, [&controlFanoutOrder, paneIndex](const QByteArray &) {
                    controlFanoutOrder.append(
                        QStringLiteral("csi:%1").arg(paneIndex));
                });
        connect(controller, &TerminalController::resetTerminalRequested,
                this, [&controlFanoutOrder, paneIndex] {
                    controlFanoutOrder.append(
                        QStringLiteral("reset:%1").arg(paneIndex));
                });
    }

    const int currentIndex = workspace.currentIndex();
    QVector<PaneId> activePaneIds;
    activePaneIds.reserve(workspace.tabCount());
    for (int index = 0; index < workspace.tabCount(); ++index) {
        activePaneIds.append(workspace.tabModel()->entryAt(index)->activePaneId);
    }
    QCOMPARE(std::ranges::count_if(
                 controllers,
                 [](const TerminalController *controller) {
                     return controller->mouseReportingEnabled();
                 }),
             1);
    QVERIFY(configuredSourceController->mouseReportingEnabled());

    LaunchOptions enabledOptions = options;
    enabledOptions.mouseReporting = true;
    workspace.applyLaunchOptions(enabledOptions);
    QVERIFY(std::ranges::all_of(
        controllers,
        [](const TerminalController *controller) {
            return controller->mouseReportingEnabled();
        }));

    // Existing panes remain independent until a reload. Reapplying the same
    // configured value still removes a local override.
    QVERIFY(configuredSource->executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(!configuredSourceController->mouseReportingEnabled());
    QCOMPARE(std::ranges::count_if(
                 controllers,
                 [](const TerminalController *controller) {
                     return controller->mouseReportingEnabled();
                 }),
             2);
    workspace.applyLaunchOptions(enabledOptions);
    QVERIFY(std::ranges::all_of(
        controllers,
        [](const TerminalController *controller) {
            return controller->mouseReportingEnabled();
        }));

    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(std::ranges::none_of(
        controllers,
        [](const TerminalController *controller) {
            return controller->mouseReportingEnabled();
        }));
    QCOMPARE(workspace.currentIndex(), currentIndex);
    for (int index = 0; index < workspace.tabCount(); ++index) {
        QCOMPARE(workspace.tabModel()->entryAt(index)->activePaneId,
                 activePaneIds.at(index));
    }
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(std::ranges::all_of(
        controllers,
        [](const TerminalController *controller) {
            return controller->mouseReportingEnabled();
        }));
    QVERIFY(!workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_mouse_reporting:")));

    // Selection-dependent actions always reach each worker. The worker owns
    // the live VT state and decides whether its correlated request performed;
    // a stale GUI selection cache cannot suppress or falsely accept them.
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("adjust_selection:left")));
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("scroll_to_selection")));
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("search_selection")));
    for (qsizetype i = 0; i < panes.size(); ++i) {
        const auto &adjustment =
            adjustmentActionSpies.at(static_cast<std::size_t>(i));
        QCOMPARE(adjustment->count(), 1);
        QVERIFY(adjustment->constFirst().at(0).toULongLong() != 0);
        QCOMPARE(
            qvariant_cast<TerminalSelectionAdjustment>(
                adjustment->constFirst().at(1)),
            TerminalSelectionAdjustment::Left);

        const auto &scroll =
            scrollToSelectionSpies.at(static_cast<std::size_t>(i));
        QCOMPARE(scroll->count(), 1);
        QVERIFY(scroll->constFirst().constFirst().toULongLong() != 0);

        const auto &search =
            searchSelectionSpies.at(static_cast<std::size_t>(i));
        QCOMPARE(search->count(), 1);
        QVERIFY(search->constFirst().constFirst().toULongLong() != 0);
    }

    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("scroll_page_lines:3")));
    for (const std::unique_ptr<QSignalSpy> &spy : scrollSpies) {
        QCOMPARE(spy->count(), 1);
        const TerminalViewportRequest request =
            qvariant_cast<TerminalViewportRequest>(spy->front().at(0));
        QCOMPARE(request.kind, TerminalViewportRequest::Kind::Delta);
        QCOMPARE(request.delta, 3);
    }

    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("select_all")));
    for (const std::unique_ptr<QSignalSpy> &spy : selectAllSpies) {
        QCOMPARE(spy->count(), 1);
    }

    // Broad action chains are action-major: every pane receives the complete
    // CSI action exactly once before any pane receives the following reset.
    GhosttyApplicationKeybindings applicationBindings(options, false);
    applicationBindings.registerWorkspace(&workspace);
    const QStringList rawBroadActions{
        QStringLiteral("csi:9:detail"),
        QStringLiteral("reset"),
    };
    applicationBindings.dispatchBroadActions(rawBroadActions);
    QCOMPARE(controlFanoutOrder.size(), 2 * panes.size());
    for (qsizetype index = 0; index < panes.size(); ++index) {
        QVERIFY(controlFanoutOrder.at(index).startsWith(
            QLatin1StringView("csi:")));
        QVERIFY(controlFanoutOrder.at(index + panes.size()).startsWith(
            QLatin1StringView("reset:")));
        QCOMPARE(csiSpies.at(static_cast<std::size_t>(index))->count(), 1);
        QCOMPARE(csiSpies.at(static_cast<std::size_t>(index))
                     ->constFirst().constFirst().toByteArray(),
                 QByteArrayLiteral("9:detail"));
        QCOMPARE(resetSpies.at(static_cast<std::size_t>(index))->count(), 1);
    }

    // The runtime path carries the compiled, owning chain emitted by a pane.
    // Reusing the same instance must preserve both its decoded payload and its
    // action-major ordering without reparsing or consuming cached values.
    const GhosttyCompiledActionChain typedBroadActions =
        GhosttyActionCatalog::compileActionChain(rawBroadActions);
    controlFanoutOrder.clear();
    for (qsizetype index = 0; index < panes.size(); ++index) {
        csiSpies.at(static_cast<std::size_t>(index))->clear();
        resetSpies.at(static_cast<std::size_t>(index))->clear();
    }

    configuredSource->broadActionsRequested(typedBroadActions);
    configuredSource->broadActionsRequested(typedBroadActions);

    constexpr qsizetype passCount = 2;
    constexpr qsizetype actionsPerPass = 2;
    QCOMPARE(controlFanoutOrder.size(),
             passCount * actionsPerPass * panes.size());
    for (qsizetype pass = 0; pass < passCount; ++pass) {
        const qsizetype passOffset =
            pass * actionsPerPass * panes.size();
        for (qsizetype index = 0; index < panes.size(); ++index) {
            QVERIFY(controlFanoutOrder.at(passOffset + index).startsWith(
                QLatin1StringView("csi:")));
            QVERIFY(controlFanoutOrder.at(
                passOffset + panes.size() + index).startsWith(
                    QLatin1StringView("reset:")));
        }
    }
    for (qsizetype index = 0; index < panes.size(); ++index) {
        const auto &csiSpy =
            csiSpies.at(static_cast<std::size_t>(index));
        QCOMPARE(csiSpy->count(), 2);
        for (const QList<QVariant> &arguments : *csiSpy) {
            QCOMPARE(arguments.constFirst().toByteArray(),
                     QByteArrayLiteral("9:detail"));
        }
        QCOMPARE(resetSpies.at(static_cast<std::size_t>(index))->count(), 2);
    }
}

void TerminalWorkspaceTest::routesWindowStateActionsToHostWindows()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    // Before the deferred initial tab is created, broad surface dispatch has
    // no target and must not manufacture a host-window transition.
    {
        TerminalWorkspace emptyWorkspace;
        QSignalSpy emptyFullscreenRequested(
            &emptyWorkspace,
            &TerminalWorkspace::toggleFullscreenRequested);
        QSignalSpy emptyMaximizeRequested(
            &emptyWorkspace,
            &TerminalWorkspace::toggleMaximizeRequested);
        QSignalSpy emptyDecorationChanged(
            &emptyWorkspace, &TerminalWorkspace::windowDecorationChanged);
        QVERIFY(!emptyWorkspace.executeSurfaceActionOnAllPanes(
            QStringLiteral("toggle_fullscreen")));
        QVERIFY(!emptyWorkspace.executeSurfaceActionOnAllPanes(
            QStringLiteral("toggle_maximize")));
        QVERIFY(!emptyWorkspace.executeSurfaceActionOnAllPanes(
            QStringLiteral("toggle_window_decorations")));
        QCOMPARE(emptyFullscreenRequested.count(), 0);
        QCOMPARE(emptyMaximizeRequested.count(), 0);
        QCOMPARE(emptyDecorationChanged.count(), 0);
    }

    // A populated workspace still cannot mutate window state until it is
    // attached to a host QQuickWindow.
    {
        TerminalWorkspace unattachedWorkspace;
        QTRY_COMPARE_WITH_TIMEOUT(unattachedWorkspace.tabCount(), 1, 1000);
        const TabListEntry *const unattachedEntry =
            unattachedWorkspace.tabModel()->entryAt(0);
        QVERIFY(unattachedEntry != nullptr);
        QSignalSpy unattachedFullscreenRequested(
            &unattachedWorkspace,
            &TerminalWorkspace::toggleFullscreenRequested);
        QSignalSpy unattachedMaximizeRequested(
            &unattachedWorkspace,
            &TerminalWorkspace::toggleMaximizeRequested);
        QSignalSpy unattachedDecorationChanged(
            &unattachedWorkspace, &TerminalWorkspace::windowDecorationChanged);
        QVERIFY(!unattachedWorkspace.dispatchAction({
            WorkspaceAction::ToggleFullscreen,
            {unattachedEntry->id, unattachedEntry->activePaneId, 0},
        }));
        QVERIFY(!unattachedWorkspace.dispatchAction({
            WorkspaceAction::ToggleMaximize,
            {unattachedEntry->id, unattachedEntry->activePaneId, 0},
        }));
        QVERIFY(!unattachedWorkspace.dispatchAction({
            WorkspaceAction::ToggleWindowDecorations,
            {unattachedEntry->id, unattachedEntry->activePaneId, 0},
        }));
        QVERIFY(!unattachedWorkspace.executeSurfaceActionOnAllPanes(
            QStringLiteral("toggle_fullscreen")));
        QVERIFY(!unattachedWorkspace.executeSurfaceActionOnAllPanes(
            QStringLiteral("toggle_maximize")));
        QVERIFY(!unattachedWorkspace.executeSurfaceActionOnAllPanes(
            QStringLiteral("toggle_window_decorations")));
        QCOMPARE(unattachedFullscreenRequested.count(), 0);
        QCOMPARE(unattachedMaximizeRequested.count(), 0);
        QCOMPARE(unattachedDecorationChanged.count(), 0);
    }

    QQuickWindow window;
    window.resize(900, 600);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    window.show();

    QTRY_COMPARE_WITH_TIMEOUT(workspace->window(), &window, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);
    const TabListEntry *entry = workspace->tabModel()->entryAt(0);
    QVERIFY(entry != nullptr);

    QSignalSpy fullscreenRequested(
        workspace.get(), &TerminalWorkspace::toggleFullscreenRequested);
    QSignalSpy maximizeRequested(
        workspace.get(), &TerminalWorkspace::toggleMaximizeRequested);
    QSignalSpy decorationChanged(workspace.get(),
                                 &TerminalWorkspace::windowDecorationChanged);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleFullscreen,
        {entry->id, entry->activePaneId, 0},
    }));
    QCOMPARE(fullscreenRequested.count(), 1);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleMaximize,
        {entry->id, entry->activePaneId, 0},
    }));
    QCOMPARE(maximizeRequested.count(), 1);
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::Auto);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleWindowDecorations,
        {entry->id, entry->activePaneId, 0},
    }));
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::None);
    QCOMPARE(decorationChanged.count(), 1);

    QVERIFY(!workspace->dispatchAction({
        WorkspaceAction::ToggleFullscreen,
        {entry->id, PaneId(999'999), 0},
    }));
    QVERIFY(!workspace->dispatchAction({
        WorkspaceAction::ToggleMaximize,
        {TabId(999'999), entry->activePaneId, 0},
    }));
    QVERIFY(!workspace->dispatchAction({
        WorkspaceAction::ToggleWindowDecorations,
        {entry->id, PaneId(999'999), 0},
    }));
    QCOMPARE(fullscreenRequested.count(), 1);
    QCOMPARE(maximizeRequested.count(), 1);
    QCOMPARE(decorationChanged.count(), 1);

    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleFullscreen,
        {},
    }));
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleMaximize,
        {},
    }));
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleWindowDecorations,
        {},
    }));
    QCOMPARE(fullscreenRequested.count(), 2);
    QCOMPARE(maximizeRequested.count(), 2);
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::Auto);
    QCOMPARE(decorationChanged.count(), 2);

    workspace->splitRight();
    workspace->newTab();
    workspace->splitRight();
    QCOMPARE(workspace->findChildren<TerminalPane *>().size(), 4);

    fullscreenRequested.clear();
    QVERIFY(workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_fullscreen")));
    QCOMPARE(fullscreenRequested.count(), 1);
    QVERIFY(!workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_fullscreen:")));
    QCOMPARE(fullscreenRequested.count(), 1);
    QVERIFY(workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_fullscreen")));
    QCOMPARE(fullscreenRequested.count(), 2);

    maximizeRequested.clear();
    QVERIFY(workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_maximize")));
    QCOMPARE(maximizeRequested.count(), 1);
    QVERIFY(!workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_maximize:")));
    QCOMPARE(maximizeRequested.count(), 1);
    QVERIFY(workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_maximize")));
    QCOMPARE(maximizeRequested.count(), 2);

    decorationChanged.clear();
    QVERIFY(workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_window_decorations")));
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::None);
    QCOMPARE(decorationChanged.count(), 1);
    QVERIFY(!workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_window_decorations:")));
    QCOMPARE(decorationChanged.count(), 1);

    // An override masks reload. The second toggle clears it rather than
    // inverting its current value, revealing the newest configured mode.
    LaunchOptions reloadedOptions = options;
    reloadedOptions.windowDecoration = WindowDecorationMode::Server;
    workspace->applyLaunchOptions(reloadedOptions);
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::None);
    QCOMPARE(decorationChanged.count(), 1);
    QVERIFY(workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_window_decorations")));
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::Server);
    QCOMPARE(decorationChanged.count(), 2);

    reloadedOptions.windowDecoration = WindowDecorationMode::None;
    workspace->applyLaunchOptions(reloadedOptions);
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::None);
    QCOMPARE(decorationChanged.count(), 3);
    QVERIFY(workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_window_decorations")));
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::Auto);
    QCOMPARE(decorationChanged.count(), 4);
    reloadedOptions.windowDecoration = WindowDecorationMode::Client;
    workspace->applyLaunchOptions(reloadedOptions);
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::Auto);
    QCOMPARE(decorationChanged.count(), 4);
    QVERIFY(workspace->executeSurfaceActionOnAllPanes(
        QStringLiteral("toggle_window_decorations")));
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::Client);
    QCOMPARE(decorationChanged.count(), 5);

    // Process-wide all/global dispatch visits every registered workspace,
    // while each multi-pane host receives exactly one state transition.
    QQuickWindow secondWindow;
    secondWindow.resize(700, 500);
    auto secondWorkspace = std::make_unique<TerminalWorkspace>();
    secondWorkspace->setParentItem(secondWindow.contentItem());
    secondWorkspace->setSize(secondWindow.size());
    secondWindow.show();
    QTRY_COMPARE_WITH_TIMEOUT(secondWorkspace->window(), &secondWindow, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(secondWorkspace->tabCount(), 1, 1000);

    QSignalSpy secondFullscreenRequested(
        secondWorkspace.get(),
        &TerminalWorkspace::toggleFullscreenRequested);
    QSignalSpy secondMaximizeRequested(
        secondWorkspace.get(),
        &TerminalWorkspace::toggleMaximizeRequested);
    QSignalSpy secondDecorationChanged(
        secondWorkspace.get(), &TerminalWorkspace::windowDecorationChanged);
    GhosttyApplicationKeybindings applicationBindings(options, false);
    applicationBindings.registerWorkspace(workspace.get());
    applicationBindings.registerWorkspace(secondWorkspace.get());

    fullscreenRequested.clear();
    maximizeRequested.clear();
    applicationBindings.dispatchBroadActions(
        {QStringLiteral("toggle_maximize")});
    QCOMPARE(maximizeRequested.count(), 1);
    QCOMPARE(secondMaximizeRequested.count(), 1);
    applicationBindings.dispatchBroadActions(
        {QStringLiteral("toggle_fullscreen")});
    QCOMPARE(fullscreenRequested.count(), 1);
    QCOMPARE(secondFullscreenRequested.count(), 1);
    decorationChanged.clear();
    applicationBindings.dispatchBroadActions(
        {QStringLiteral("toggle_window_decorations")});
    QCOMPARE(decorationChanged.count(), 1);
    QCOMPARE(secondDecorationChanged.count(), 1);
    QCOMPARE(workspace->windowDecoration(), WindowDecorationMode::None);
    QCOMPARE(secondWorkspace->windowDecoration(), WindowDecorationMode::None);

    secondWorkspace.reset();
    secondWindow.close();
    workspace.reset();
    window.close();
}

void TerminalWorkspaceTest::splitDirectionsPlaceAndFocusNewPane_data()
{
    QTest::addColumn<QString>("serialized");
    QTest::addColumn<QSizeF>("workspaceSize");
    QTest::addColumn<Qt::Orientation>("orientation");
    QTest::addColumn<bool>("placeNewPaneFirst");

    const QSizeF wide(902.0, 602.0);
    const QSizeF tall(602.0, 902.0);
    const QSizeF square(602.0, 602.0);
    QTest::newRow("left")
        << QStringLiteral("new_split:left") << wide
        << Qt::Horizontal << true;
    QTest::newRow("right")
        << QStringLiteral("new_split:right") << wide
        << Qt::Horizontal << false;
    QTest::newRow("up")
        << QStringLiteral("new_split:up") << tall
        << Qt::Vertical << true;
    QTest::newRow("down")
        << QStringLiteral("new_split:down") << tall
        << Qt::Vertical << false;
    QTest::newRow("explicit-auto-wide")
        << QStringLiteral("new_split:auto") << wide
        << Qt::Horizontal << false;
    QTest::newRow("explicit-auto-tall")
        << QStringLiteral("new_split:auto") << tall
        << Qt::Vertical << false;
    QTest::newRow("explicit-auto-square")
        << QStringLiteral("new_split:auto") << square
        << Qt::Vertical << false;
    QTest::newRow("default-auto-wide")
        << QStringLiteral("new_split") << wide
        << Qt::Horizontal << false;
    QTest::newRow("default-auto-square")
        << QStringLiteral("new_split") << square
        << Qt::Vertical << false;
}

void TerminalWorkspaceTest::splitDirectionsPlaceAndFocusNewPane()
{
    QFETCH(QString, serialized);
    QFETCH(QSizeF, workspaceSize);
    QFETCH(Qt::Orientation, orientation);
    QFETCH(bool, placeNewPaneFirst);

    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(workspaceSize);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabListEntry *before = workspace.tabModel()->entryAt(0);
    QVERIFY(before != nullptr);
    const PaneId originalId = before->activePaneId;
    TerminalPane *originalPane = workspace.findChild<TerminalPane *>();
    QVERIFY(originalPane != nullptr);

    QVERIFY(originalPane->executeConfiguredAction(serialized));
    const QList<TerminalPane *> panes =
        workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 2);
    TerminalPane *newPane = panes.at(0) == originalPane
        ? panes.at(1)
        : panes.at(0);
    QVERIFY(newPane != nullptr);

    const TabListEntry *after = workspace.tabModel()->entryAt(0);
    QVERIFY(after != nullptr);
    QVERIFY(after->activePaneId != originalId);
    if (orientation == Qt::Horizontal) {
        QVERIFY(qAbs(originalPane->height() - newPane->height()) <= 1.0);
        QVERIFY(qAbs(originalPane->width() - newPane->width()) <= 1.0);
        QCOMPARE(qRound(originalPane->y()), qRound(newPane->y()));
        QVERIFY(placeNewPaneFirst
                    ? newPane->x() < originalPane->x()
                    : newPane->x() > originalPane->x());
    } else {
        QVERIFY(qAbs(originalPane->width() - newPane->width()) <= 1.0);
        QVERIFY(qAbs(originalPane->height() - newPane->height()) <= 1.0);
        QCOMPARE(qRound(originalPane->x()), qRound(newPane->x()));
        QVERIFY(placeNewPaneFirst
                    ? newPane->y() < originalPane->y()
                    : newPane->y() > originalPane->y());
    }
}

void TerminalWorkspaceTest::automaticSplitUsesOriginatingPaneAspect()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(1002.0, 702.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *leftPane = workspace.findChild<TerminalPane *>();
    QVERIFY(leftPane != nullptr);

    // The root is wider than tall, so the first default auto split goes right.
    QVERIFY(leftPane->executeConfiguredAction(QStringLiteral("new_split")));
    QList<TerminalPane *> panes = workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 2);
    TerminalPane *rightPane = panes.at(0) == leftPane
        ? panes.at(1)
        : panes.at(0);
    QVERIFY(rightPane->x() > leftPane->x());
    QVERIFY(rightPane->width() < rightPane->height());

    // Auto is resolved from that originating child, not the wider workspace,
    // so its new sibling is placed below it.
    QVERIFY(rightPane->executeConfiguredAction(QStringLiteral("new_split")));
    panes = workspace.findChildren<TerminalPane *>();
    QCOMPARE(panes.size(), 3);
    TerminalPane *lowerPane = nullptr;
    for (TerminalPane *candidate : panes) {
        if (candidate != leftPane && candidate != rightPane) {
            lowerPane = candidate;
        }
    }
    QVERIFY(lowerPane != nullptr);
    QCOMPARE(qRound(lowerPane->x()), qRound(rightPane->x()));
    QVERIFY(lowerPane->y() > rightPane->y());
}

void TerminalWorkspaceTest::indexedLastAndMovedTabsPreserveStableIds()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId first = workspace.tabModel()->idAt(0);
    const PaneId firstPane = workspace.tabModel()->entryAt(0)->activePaneId;

    workspace.newTab();
    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 3);
    const TabId second = workspace.tabModel()->idAt(1);
    const TabId third = workspace.tabModel()->idAt(2);
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(third.isValid());
    QVERIFY(first != second && second != third && first != third);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateTabByIndex,
        {TabId{}, PaneId{}, 1},
    }));
    QCOMPARE(workspace.currentIndex(), 0);
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), first);

    // A high one-based index clamps to the final tab.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateTabByIndex,
        {TabId{}, PaneId{}, 999},
    }));
    QCOMPARE(workspace.currentIndex(), 2);
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), third);
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ActivateLastTab,
        {},
    }));
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ActivateTabByIndex,
        {TabId{}, PaneId{}, 0},
    }));

    // The parser accepts usize, but Ghostty's execution boundary is c_int.
    // Start away from the final tab so an old clamp-to-final implementation
    // cannot make this rejection assertion pass accidentally.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateTabByIndex,
        {TabId{}, PaneId{}, 2},
    }));
    const TabId selectedBeforeOversizedIndex =
        workspace.tabModel()->idAt(workspace.currentIndex());
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ActivateTabByIndex,
        {TabId{}, PaneId{},
         static_cast<qint64>(std::numeric_limits<int>::max()) + 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             selectedBeforeOversizedIndex);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateLastTab,
        {},
    }));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), third);

    // Move the first tab forward without changing the selected tab identity.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {first, firstPane, 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(0), second);
    QCOMPARE(workspace.tabModel()->idAt(1), first);
    QCOMPARE(workspace.tabModel()->idAt(2), third);
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), third);

    // Moving the selected final tab forward wraps it to the front and updates
    // only its row index; the stable TabId remains selected.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {third, PaneId{}, 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(0), third);
    QCOMPARE(workspace.tabModel()->idAt(1), second);
    QCOMPARE(workspace.tabModel()->idAt(2), first);
    QCOMPARE(workspace.currentIndex(), 0);
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), third);

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {third, PaneId{}, 3},
    }));
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {TabId{}, PaneId(999'999), 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(0), third);
    QCOMPARE(workspace.tabModel()->idAt(1), second);
    QCOMPARE(workspace.tabModel()->idAt(2), first);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateLastTab,
        {},
    }));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()), first);
}

void TerminalWorkspaceTest::surfaceBaseTitlesFollowStablePanesAndOscUpdates()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/surface-base-title-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString firstRelease =
        directory.filePath(QStringLiteral("first-release"));
    const QString secondRelease =
        directory.filePath(QStringLiteral("second-release"));

    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.workingDirectory = directory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "while [ ! -e \"$1\" ]; do sleep 0.02; done; "
            "printf 'selection-marker\\r\\n\\033]0;cached-title\\007'; "
            "while [ ! -e \"$2\" ]; do sleep 0.02; done; "
            "printf '\\033]0;cached-title\\007'; sleep 30"),
        QStringLiteral("surface-title-test"),
        firstRelease,
        secondRelease,
    };
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(900, 600);
    TerminalWorkspace workspace;
    workspace.setParentItem(window.contentItem());
    workspace.setSize(window.size());
    window.show();
    QTRY_COMPARE_WITH_TIMEOUT(workspace.window(), &window, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId firstPaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    auto *firstController =
        firstPane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QVERIFY(!firstController->hasTitle());
    QCOMPARE(firstPane->title(), QStringLiteral("sh"));

    // A required-but-empty payload is a present, empty base title rather
    // than the absence that selects the launch fallback.
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_surface_title:")));
    QVERIFY(firstController->hasTitle());
    QVERIFY(firstPane->title().isEmpty());
    QVERIFY(workspace.tabModel()->entryAt(0)->title.isEmpty());
    QVERIFY(workspace.currentTitle().isEmpty());

    QVERIFY(firstPane->executeConfiguredAction(QStringLiteral(
        R"(set_surface_title:\xf0\x9f\x91\xbb base:first)")));
    QCOMPARE(firstPane->title(), QStringLiteral("👻 base:first"));
    QCOMPARE(workspace.currentTitle(), QStringLiteral("👻 base:first"));

    QFile release(firstRelease);
    QVERIFY(release.open(QIODevice::WriteOnly));
    release.close();
    QTRY_COMPARE_WITH_TIMEOUT(firstPane->title(),
                              QStringLiteral("cached-title"), 3000);

    // The tab override masks base-title updates without freezing them.
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_tab_title:tab mask")));
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_surface_title:action-base")));
    QCOMPARE(firstPane->title(), QStringLiteral("action-base"));
    QCOMPARE(workspace.tabModel()->entryAt(0)->title,
             QStringLiteral("action-base"));
    QCOMPARE(workspace.currentTitle(), QStringLiteral("tab mask"));

    // The second OSC deliberately restores the value cached before the
    // action. The GUI base cache must still publish it as a replacement.
    release.setFileName(secondRelease);
    QVERIFY(release.open(QIODevice::WriteOnly));
    release.close();
    QTRY_COMPARE_WITH_TIMEOUT(firstPane->title(),
                              QStringLiteral("cached-title"), 3000);
    QCOMPARE(workspace.currentTitle(), QStringLiteral("tab mask"));
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_tab_title:")));
    QCOMPARE(workspace.currentTitle(), QStringLiteral("cached-title"));

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstPaneId, 0},
    }));
    const PaneId secondPaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(secondPaneId.isValid());
    QVERIFY(secondPaneId != firstPaneId);
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane != firstPane) secondPane = pane;
    }
    QVERIFY(secondPane != nullptr);
    auto *secondController =
        secondPane->findChild<TerminalController *>();
    QVERIFY(secondController != nullptr);
    const QString activeTitle = workspace.currentTitle();

    // The source pane remains authoritative while inactive; changing it does
    // not activate the pane or replace the tab's active-surface title.
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_surface_title:inactive source")));
    QCOMPARE(firstPane->title(), QStringLiteral("inactive source"));
    QCOMPARE(workspace.currentTitle(), activeTitle);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, secondPaneId);

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::SetSurfaceTitle,
        {TabId(999'999), firstPaneId, 0},
        QStringLiteral("mismatched"),
    }));
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::SetSurfaceTitle,
        {tabId, PaneId{}, 0},
        QStringLiteral("missing pane"),
    }));
    QCOMPARE(firstPane->title(), QStringLiteral("inactive source"));

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("select_all")));
    QTRY_VERIFY_WITH_TIMEOUT(firstController->selectionAvailable(), 1000);
    secondPane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), secondPane, 1000);

    QSignalSpy firstTitleChanges(firstPane, &TerminalPane::titleChanged);
    QSignalSpy secondTitleChanges(secondPane, &TerminalPane::titleChanged);
    const int selectedIndex = workspace.currentIndex();
    const bool firstSelection = firstController->selectionAvailable();
    const bool secondSelection = secondController->selectionAvailable();
    QQuickItem *const focusedItem = window.activeFocusItem();
    GhosttyApplicationKeybindings applicationBindings({}, false);
    applicationBindings.registerWorkspace(&workspace);
    applicationBindings.dispatchBroadActions({QStringLiteral(
        R"(set_surface_title:\xf0\x9f\x8c\x90 broad)")});
    QCOMPARE(firstTitleChanges.count(), 1);
    QCOMPARE(secondTitleChanges.count(), 1);
    QCOMPARE(firstPane->title(), QStringLiteral("🌐 broad"));
    QCOMPARE(secondPane->title(), QStringLiteral("🌐 broad"));
    QCOMPARE(workspace.currentIndex(), selectedIndex);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, secondPaneId);
    QCOMPARE(firstController->selectionAvailable(), firstSelection);
    QCOMPARE(secondController->selectionAvailable(), secondSelection);
    QCOMPARE(window.activeFocusItem(), focusedItem);

    applicationBindings.dispatchBroadActions(
        {QStringLiteral(R"(set_surface_title:\xff)")});
    QCOMPARE(firstTitleChanges.count(), 1);
    QCOMPARE(secondTitleChanges.count(), 1);

    // Title copying consumes only each pane's raw surface layer. It neither
    // exposes the tab override nor disturbs focus, selection, or primary.
    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const bool supportsPrimary = clipboard->supportsSelection();
    const QString standardSentinel = QStringLiteral("standard sentinel");
    const QString primarySentinel = QStringLiteral("primary sentinel");
    const QString copyAction = QStringLiteral("copy_title_to_clipboard");
    const QString firstCopyTitle = QStringLiteral("  first 👻  ");
    const QString secondCopyTitle = QStringLiteral("  second 🌐  ");
    QStringList clipboardWrites;
    const QMetaObject::Connection clipboardConnection = connect(
        clipboard, &QClipboard::changed, &workspace,
        [&clipboardWrites, clipboard](QClipboard::Mode mode) {
            if (mode == QClipboard::Clipboard) {
                clipboardWrites.append(
                    clipboard->text(QClipboard::Clipboard));
            }
        });
    const auto resetClipboards = [&] {
        clipboard->setText(standardSentinel, QClipboard::Clipboard);
        if (supportsPrimary) {
            clipboard->setText(primarySentinel, QClipboard::Selection);
        }
        clipboardWrites.clear();
    };
    const auto verifyInteractionState = [&] {
        QCOMPARE(workspace.currentIndex(), selectedIndex);
        QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId,
                 secondPaneId);
        QCOMPARE(firstController->selectionAvailable(), firstSelection);
        QCOMPARE(secondController->selectionAvailable(), secondSelection);
        QCOMPARE(window.activeFocusItem(), focusedItem);
        if (supportsPrimary) {
            QCOMPARE(clipboard->text(QClipboard::Selection), primarySentinel);
        }
    };

    firstPane->setSurfaceTitle(firstCopyTitle);
    secondPane->setSurfaceTitle(QStringLiteral("hidden second base"));
    secondPane->setSurfaceTitleOverride(
        std::optional<QString>{secondCopyTitle});
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_tab_title:tab copy mask")));
    QCOMPARE(workspace.currentTitle(), QStringLiteral("tab copy mask"));

    resetClipboards();
    QVERIFY(firstPane->executeConfiguredAction(copyAction));
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard), firstCopyTitle);
    verifyInteractionState();
    QVERIFY(secondPane->executeConfiguredAction(copyAction));
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard), secondCopyTitle);
    verifyInteractionState();

    // Broad fanout overwrites in its stable pane order. Empty later panes are
    // no-ops, and an all-empty fanout reports that nothing was performed.
    resetClipboards();
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(copyAction));
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard), secondCopyTitle);
    QCOMPARE(clipboardWrites,
             QStringList({firstCopyTitle, secondCopyTitle}));
    verifyInteractionState();

    secondPane->setSurfaceTitleOverride(std::nullopt);
    secondPane->setSurfaceTitle(QString{});
    resetClipboards();
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(copyAction));
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard), firstCopyTitle);
    QCOMPARE(clipboardWrites, QStringList({firstCopyTitle}));
    verifyInteractionState();

    firstPane->setSurfaceTitle(QString{});
    resetClipboards();
    QVERIFY(!workspace.executeSurfaceActionOnAllPanes(copyAction));
    QCOMPARE(clipboard->text(QClipboard::Clipboard), standardSentinel);
    QVERIFY(clipboardWrites.isEmpty());
    verifyInteractionState();

    disconnect(clipboardConnection);
    clipboard->clear(QClipboard::Clipboard);
    if (supportsPrimary) {
        clipboard->clear(QClipboard::Selection);
    }

    QPointer<TerminalPane> removedPane(firstPane);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ClosePane,
        {tabId, firstPaneId, 0},
    }));
    QTRY_VERIFY_WITH_TIMEOUT(removedPane.isNull(), 1000);
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::SetSurfaceTitle,
        {tabId, firstPaneId, 0},
        QStringLiteral("stale"),
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, secondPaneId);
}

void TerminalWorkspaceTest::tabTitleOverridesFollowStableSourcesAndReset()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "sleep 1; printf '\\033]0;underlying-title\\007'; sleep 30"),
    };
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId firstTabId = workspace.tabModel()->idAt(0);
    const PaneId firstPaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);

    const auto tabIndex = [&workspace](TabId id) {
        return workspace.tabModel()->indexOf(id);
    };
    const auto entry = [&workspace, &tabIndex](TabId id) {
        return workspace.tabModel()->entryAt(tabIndex(id));
    };
    const auto displayedTitle = [&workspace, &tabIndex](TabId id) {
        const int index = tabIndex(id);
        return workspace.tabModel()
            ->data(workspace.tabModel()->index(index, 0),
                   TabListModel::TitleRole)
            .toString();
    };

    QSignalSpy currentTitleChanged(
        &workspace, &TerminalWorkspace::currentTitleChanged);
    QVERIFY(firstPane->executeConfiguredAction(QStringLiteral(
        R"(set_tab_title:\xf0\x9f\x91\xbb project:first)")));
    QCOMPARE(entry(firstTabId)->titleOverride,
             QStringLiteral("👻 project:first"));
    QCOMPARE(displayedTitle(firstTabId),
             QStringLiteral("👻 project:first"));
    QCOMPARE(workspace.currentTitle(),
             QStringLiteral("👻 project:first"));
    QCOMPARE(currentTitleChanged.count(), 1);

    // Terminal-originated title changes continue updating the base title
    // underneath the stable override.
    QTRY_COMPARE_WITH_TIMEOUT(firstPane->title(),
                              QStringLiteral("underlying-title"), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(entry(firstTabId)->title,
                              QStringLiteral("underlying-title"), 3000);
    QCOMPARE(displayedTitle(firstTabId),
             QStringLiteral("👻 project:first"));

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {firstTabId, firstPaneId, 0},
    }));
    const PaneId secondPaneId = entry(firstTabId)->activePaneId;
    QVERIFY(secondPaneId.isValid());
    QVERIFY(secondPaneId != firstPaneId);
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane != firstPane) secondPane = pane;
    }
    QVERIFY(secondPane != nullptr);
    QCOMPARE(entry(firstTabId)->titleOverride,
             QStringLiteral("👻 project:first"));
    QCOMPARE(entry(firstTabId)->title, secondPane->title());
    QCOMPARE(displayedTitle(firstTabId),
             QStringLiteral("👻 project:first"));

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    const TabId secondTabId =
        workspace.tabModel()->idAt(workspace.currentIndex());
    QVERIFY(secondTabId != firstTabId);
    const QString secondTabTitleBefore = workspace.currentTitle();
    const int currentSignalsBeforeInactiveUpdate =
        currentTitleChanged.count();

    // The originating pane owns the target context even while its tab is
    // inactive. The selected tab and its window title do not drift.
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_tab_title:inactive source")));
    QCOMPARE(entry(firstTabId)->titleOverride,
             QStringLiteral("inactive source"));
    QVERIFY(entry(secondTabId)->titleOverride.isEmpty());
    QCOMPARE(workspace.currentTitle(), secondTabTitleBefore);
    QCOMPARE(currentTitleChanged.count(),
             currentSignalsBeforeInactiveUpdate);

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::SetTabTitle,
        {secondTabId, firstPaneId, 0},
        QStringLiteral("mismatched"),
    }));
    QCOMPARE(entry(firstTabId)->titleOverride,
             QStringLiteral("inactive source"));
    QVERIFY(entry(secondTabId)->titleOverride.isEmpty());

    // Insertion and row movement retain the override by stable TabId.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {firstTabId, firstPaneId, 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(1), firstTabId);
    QCOMPARE(entry(firstTabId)->titleOverride,
             QStringLiteral("inactive source"));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             secondTabId);

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_tab_title:")));
    QVERIFY(entry(firstTabId)->titleOverride.isEmpty());
    QCOMPARE(displayedTitle(firstTabId), entry(firstTabId)->title);
    QCOMPARE(workspace.currentTitle(), secondTabTitleBefore);

    GhosttyApplicationKeybindings applicationBindings({}, false);
    applicationBindings.registerWorkspace(&workspace);
    const TabId selectedBeforeBroad =
        workspace.tabModel()->idAt(workspace.currentIndex());
    applicationBindings.dispatchBroadActions({QStringLiteral(
        R"(set_tab_title:\xf0\x9f\x8c\x90 broad)")});
    for (int index = 0; index < workspace.tabCount(); ++index) {
        QCOMPARE(workspace.tabModel()->entryAt(index)->titleOverride,
                 QStringLiteral("🌐 broad"));
        QCOMPARE(workspace.tabModel()
                     ->data(workspace.tabModel()->index(index, 0),
                            TabListModel::TitleRole)
                     .toString(),
                 QStringLiteral("🌐 broad"));
    }
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             selectedBeforeBroad);
    QCOMPARE(workspace.currentTitle(), QStringLiteral("🌐 broad"));

    applicationBindings.dispatchBroadActions(
        {QStringLiteral(R"(set_tab_title:\xff)")});
    for (int index = 0; index < workspace.tabCount(); ++index) {
        QCOMPARE(workspace.tabModel()->entryAt(index)->titleOverride,
                 QStringLiteral("🌐 broad"));
    }

    applicationBindings.dispatchBroadActions(
        {QStringLiteral("set_tab_title:")});
    for (int index = 0; index < workspace.tabCount(); ++index) {
        QVERIFY(workspace.tabModel()->entryAt(index)->titleOverride.isEmpty());
    }
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             selectedBeforeBroad);
    QCOMPARE(workspace.currentTitle(), entry(secondTabId)->title);
}

void TerminalWorkspaceTest::bellFeedbackRoutesAcrossPaneTabWindowAndReload()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.bellFeatures = {
        .system = false,
        .audio = false,
        .attention = true,
        .title = true,
        .border = false,
    };
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(900, 600);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    QVERIFY(workspace->initialize(options));
    window.show();
    window.requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 1000);

    const CurrentTabProbe first = currentTabProbe(*workspace);
    const CurrentTabProbe active = splitRightProbe(*workspace);
    QVERIFY(first.pane != nullptr);
    QVERIFY(active.pane != nullptr);
    QCOMPARE(first.tabId, active.tabId);
    TerminalController *const firstController =
        first.pane->findChild<TerminalController *>();
    TerminalController *const activeController =
        active.pane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QVERIFY(activeController != nullptr);

    const auto tabIndex = [&workspace](TabId id) {
        return workspace->tabModel()->indexOf(id);
    };
    const auto entry = [&workspace, &tabIndex](TabId id) {
        return workspace->tabModel()->entryAt(tabIndex(id));
    };
    const auto tabRole = [&workspace, &tabIndex](TabId id, int role) {
        const int index = tabIndex(id);
        return workspace->tabModel()->data(
            workspace->tabModel()->index(index, 0), role);
    };

    const QString surfaceTitle = QStringLiteral("raw surface title");
    const QString tabTitle = QStringLiteral("raw tab override");
    active.pane->setSurfaceTitle(surfaceTitle);
    QVERIFY(active.pane->executeConfiguredAction(
        QStringLiteral("set_tab_title:raw tab override")));
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {active.tabId, active.paneId, 0},
    }));
    QCOMPARE(tabRole(active.tabId, TabListModel::TitleRole).toString(),
             QStringLiteral("🔍 raw tab override"));
    QCOMPARE(entry(active.tabId)->title, surfaceTitle);
    QCOMPARE(entry(active.tabId)->titleOverride, tabTitle);

    QSignalSpy windowAttention(workspace.get(),
                               &TerminalWorkspace::windowAttentionRequested);

    // BEL decorates the computed title without entering either raw title
    // layer. Its prefix remains outside the existing zoom decoration.
    Q_EMIT activeController->bell();
    QVERIFY(active.pane->bellRinging());
    QVERIFY(active.pane->bellTitleVisible());
    QVERIFY(!active.pane->bellBorderVisible());
    QCOMPARE(tabRole(active.tabId, TabListModel::TitleRole).toString(),
             QStringLiteral("🔔 🔍 raw tab override"));
    QCOMPARE(workspace->currentTitle(),
             QStringLiteral("🔔 🔍 raw tab override"));
    QCOMPARE(entry(active.tabId)->title, surfaceTitle);
    QCOMPARE(entry(active.tabId)->titleOverride, tabTitle);
    QVERIFY(!tabRole(active.tabId, TabListModel::AttentionRole).toBool());
    QCOMPARE(windowAttention.count(), 0);

    // A selected tab never requests tab attention for a different split, and
    // only its active pane contributes the title decoration.
    Q_EMIT firstController->bell();
    QVERIFY(first.pane->bellRinging());
    QVERIFY(!tabRole(first.tabId, TabListModel::AttentionRole).toBool());
    QCOMPARE(tabRole(first.tabId, TabListModel::TitleRole).toString(),
             QStringLiteral("🔔 🔍 raw tab override"));
    QCOMPARE(windowAttention.count(), 0);

    workspace->newTab();
    QCOMPARE(workspace->tabCount(), 2);
    const TabId secondTabId =
        workspace->tabModel()->idAt(workspace->currentIndex());
    QVERIFY(secondTabId != first.tabId);
    const int oldTabIndex = tabIndex(first.tabId);
    QVERIFY(oldTabIndex >= 0);

    // Repeated BEL is still an event even though the pane latch was already
    // set. An inactive tab records attention independently from that latch.
    Q_EMIT activeController->bell();
    QVERIFY(tabRole(first.tabId, TabListModel::AttentionRole).toBool());
    QCOMPARE(windowAttention.count(), 0);

    workspace->setCurrentIndex(oldTabIndex);
    QVERIFY(!tabRole(first.tabId, TabListModel::AttentionRole).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!active.pane->bellRinging(), 1000);
    QVERIFY(first.pane->bellRinging());

    // Host-window attention is per BEL, not per pane-latch transition.
    QQuickWindow secondWindow;
    secondWindow.resize(window.size());
    secondWindow.show();
    secondWindow.requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(secondWindow.isExposed(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(secondWindow.isActive(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isActive(), 1000);

    Q_EMIT activeController->bell();
    Q_EMIT activeController->bell();
    QCOMPARE(windowAttention.count(), 2);
    QVERIFY(active.pane->bellRinging());

    // Live reload changes only presentation and future event policy. Both
    // pane latches and the raw title layers survive it.
    LaunchOptions borderOnly = options;
    borderOnly.bellFeatures.attention = false;
    borderOnly.bellFeatures.title = false;
    borderOnly.bellFeatures.border = true;
    workspace->applyLaunchOptions(borderOnly);
    QVERIFY(active.pane->bellRinging());
    QVERIFY(first.pane->bellRinging());
    QVERIFY(!active.pane->bellTitleVisible());
    QVERIFY(active.pane->bellBorderVisible());
    QVERIFY(first.pane->bellBorderVisible());
    QCOMPARE(tabRole(first.tabId, TabListModel::TitleRole).toString(),
             QStringLiteral("🔍 raw tab override"));
    QCOMPARE(entry(first.tabId)->title, surfaceTitle);
    QCOMPARE(entry(first.tabId)->titleOverride, tabTitle);

    Q_EMIT activeController->bell();
    QCOMPARE(windowAttention.count(), 2);

    LaunchOptions titleOnly = borderOnly;
    titleOnly.bellFeatures.attention = true;
    titleOnly.bellFeatures.title = true;
    titleOnly.bellFeatures.border = false;
    workspace->applyLaunchOptions(titleOnly);
    QVERIFY(active.pane->bellRinging());
    QVERIFY(first.pane->bellRinging());
    QVERIFY(active.pane->bellTitleVisible());
    QVERIFY(!active.pane->bellBorderVisible());
    QVERIFY(!first.pane->bellBorderVisible());
    QCOMPARE(tabRole(first.tabId, TabListModel::TitleRole).toString(),
             QStringLiteral("🔔 🔍 raw tab override"));
    QCOMPARE(entry(first.tabId)->title, surfaceTitle);
    QCOMPARE(entry(first.tabId)->titleOverride, tabTitle);

    Q_EMIT activeController->bell();
    Q_EMIT activeController->bell();
    QCOMPARE(windowAttention.count(), 4);
    QVERIFY(!tabRole(first.tabId, TabListModel::AttentionRole).toBool());
}

void TerminalWorkspaceTest::bellAttentionPublicationMayDestroyWorkspace()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.bellFeatures.attention = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    const auto initializedWorkspace = [&options] {
        auto *workspace = new TerminalWorkspace;
        workspace->setSize(QSizeF(640.0, 360.0));
        if (!workspace->initialize(options,
                                   TerminalSessionStartMode::Deferred)) {
            delete workspace;
            return static_cast<TerminalWorkspace *>(nullptr);
        }
        return workspace;
    };

    // Publishing AttentionRole from a BEL may synchronously destroy the
    // workspace through a model observer.
    {
        TerminalWorkspace *workspace = initializedWorkspace();
        QVERIFY(workspace != nullptr);
        const CurrentTabProbe first = currentTabProbe(*workspace);
        QVERIFY(first.pane != nullptr);
        TerminalController *const controller =
            first.pane->findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        workspace->newTab();

        bool observedAttention = false;
        const QPointer<TerminalWorkspace> guard(workspace);
        connect(workspace->tabModel(), &QAbstractItemModel::dataChanged, this,
                [workspace, &observedAttention](const QModelIndex &,
                                                const QModelIndex &,
                                                const QList<int> &roles) {
                    if (!roles.contains(TabListModel::AttentionRole)) return;
                    observedAttention = true;
                    delete workspace;
                });
        Q_EMIT controller->bell();
        QVERIFY(observedAttention);
        QVERIFY(guard.isNull());
    }

    // Clearing that role while selecting the tab has the same destructive
    // publication boundary.
    {
        TerminalWorkspace *workspace = initializedWorkspace();
        QVERIFY(workspace != nullptr);
        const CurrentTabProbe first = currentTabProbe(*workspace);
        QVERIFY(first.pane != nullptr);
        TerminalController *const controller =
            first.pane->findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        workspace->newTab();
        Q_EMIT controller->bell();
        const int firstIndex = workspace->tabModel()->indexOf(first.tabId);
        QVERIFY(firstIndex >= 0);
        QVERIFY(workspace->tabModel()
                    ->data(workspace->tabModel()->index(firstIndex, 0),
                           TabListModel::AttentionRole)
                    .toBool());

        bool observedAttention = false;
        const QPointer<TerminalWorkspace> guard(workspace);
        connect(workspace->tabModel(), &QAbstractItemModel::dataChanged, this,
                [workspace, &observedAttention](const QModelIndex &,
                                                const QModelIndex &,
                                                const QList<int> &roles) {
                    if (!roles.contains(TabListModel::AttentionRole)) return;
                    observedAttention = true;
                    delete workspace;
                });
        workspace->setCurrentIndex(firstIndex);
        QVERIFY(observedAttention);
        QVERIFY(guard.isNull());
    }
}

void TerminalWorkspaceTest::surfaceTitlePromptsPreserveStableTargetsAndLayers()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/surface-title-prompt-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString firstRelease =
        directory.filePath(QStringLiteral("first-release"));
    const QString secondRelease =
        directory.filePath(QStringLiteral("second-release"));

    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.workingDirectory = directory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "while [ ! -e \"$1\" ]; do sleep 0.02; done; "
            "printf '\\033]0;base-a\\007'; "
            "while [ ! -e \"$2\" ]; do sleep 0.02; done; "
            "printf '\\033]0;base-a\\007'; sleep 30"),
        QStringLiteral("surface-prompt-test"),
        firstRelease,
        secondRelease,
    };
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId firstTabId = workspace.tabModel()->idAt(0);
    const PaneId firstPaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    auto *controller = firstPane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    QSignalSpy requested(
        &workspace, &TerminalWorkspace::titlePromptRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::titlePromptResolved);

    // Neither the launch fallback nor a containing-tab override seeds the
    // raw per-surface prompt when no base title exists.
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_tab_title:tab mask")));
    QCOMPARE(workspace.currentTitle(), QStringLiteral("tab mask"));
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.at(0).at(1).toString(),
             QStringLiteral("Change Terminal Title"));
    QVERIFY(requested.at(0).at(2).toString().isEmpty());
    const quint64 absentPromptId =
        requested.at(0).at(0).toULongLong();
    workspace.cancelTitlePrompt(absentPromptId);
    QCOMPARE(resolved.count(), 1);

    QFile release(firstRelease);
    QVERIFY(release.open(QIODevice::WriteOnly));
    release.close();
    QTRY_COMPARE_WITH_TIMEOUT(controller->title(), QStringLiteral("base-a"),
                              3000);
    QCOMPARE(firstPane->title(), QStringLiteral("base-a"));
    QCOMPARE(workspace.currentTitle(), QStringLiteral("tab mask"));

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QCOMPARE(requested.count(), 2);
    QCOMPARE(requested.at(1).at(2).toString(), QStringLiteral("base-a"));
    const QString override = QStringLiteral("  override 👻  ");
    const quint64 overridePromptId =
        requested.at(1).at(0).toULongLong();
    workspace.confirmTitlePrompt(overridePromptId, override);
    QCOMPARE(firstPane->surfaceTitleOverride(),
             std::optional<QString>{override});
    QCOMPARE(firstPane->title(), override);
    QCOMPARE(workspace.currentTitle(), QStringLiteral("tab mask"));

    // Reset and both base-title writers remain hidden by the persistent
    // override. The repeated OSC restores the terminal's pre-action cache.
    QSignalSpy terminalUpdates(controller,
                               &TerminalController::terminalUpdated);
    const int updatesBeforeReset = terminalUpdates.count();
    QVERIFY(firstPane->executeConfiguredAction(QStringLiteral("reset")));
    QTRY_VERIFY_WITH_TIMEOUT(
        terminalUpdates.count() > updatesBeforeReset, 1000);
    QCOMPARE(controller->title(), QStringLiteral("base-a"));
    QCOMPARE(firstPane->surfaceTitleOverride(),
             std::optional<QString>{override});
    QCOMPARE(firstPane->title(), override);

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_surface_title:base-b")));
    QCOMPARE(controller->title(), QStringLiteral("base-b"));
    QCOMPARE(firstPane->title(), override);
    release.setFileName(secondRelease);
    QVERIFY(release.open(QIODevice::WriteOnly));
    release.close();
    QTRY_COMPARE_WITH_TIMEOUT(controller->title(), QStringLiteral("base-a"),
                              3000);
    QCOMPARE(firstPane->title(), override);
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_surface_title:base-c")));
    QCOMPARE(controller->title(), QStringLiteral("base-c"));
    QCOMPARE(firstPane->title(), override);

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QCOMPARE(requested.count(), 3);
    QCOMPARE(requested.at(2).at(2).toString(), override);
    const quint64 cancelPromptId =
        requested.at(2).at(0).toULongLong();
    workspace.cancelTitlePrompt(cancelPromptId);
    QCOMPARE(firstPane->surfaceTitleOverride(),
             std::optional<QString>{override});

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_tab_title:")));
    QCOMPARE(workspace.currentTitle(), override);
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QCOMPARE(requested.count(), 4);
    const quint64 clearPromptId =
        requested.at(3).at(0).toULongLong();
    workspace.confirmTitlePrompt(clearPromptId, QString{});
    QVERIFY(!firstPane->surfaceTitleOverride().has_value());
    QCOMPARE(firstPane->title(), QStringLiteral("base-c"));
    QCOMPARE(workspace.currentTitle(), QStringLiteral("base-c"));

    // A present empty base remains distinct from the visible launch fallback,
    // while its prompt field is still correctly empty.
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_surface_title:")));
    QVERIFY(firstPane->title().isEmpty());
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QCOMPARE(requested.count(), 5);
    QVERIFY(requested.at(4).at(2).toString().isEmpty());
    workspace.cancelTitlePrompt(
        requested.at(4).at(0).toULongLong());

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::PromptSurfaceTitle,
        {TabId(999'999), firstPaneId, 0},
    }));
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::PromptSurfaceTitle,
        {firstTabId, PaneId{}, 0},
    }));
    QCOMPARE(requested.count(), 5);

    // Surface and tab prompts share one FIFO. Both snapshot their stable raw
    // target before tab selection and ordering change underneath the dialog.
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_surface_title:stable base")));
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QCOMPARE(requested.count(), 6);
    const quint64 stablePromptId =
        requested.at(5).at(0).toULongLong();
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_tab_title")));
    QCOMPARE(requested.count(), 6);

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    const TabId secondTabId =
        workspace.tabModel()->idAt(workspace.currentIndex());
    const QString secondTitle = workspace.currentTitle();
    QVERIFY(secondTabId != firstTabId);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {firstTabId, firstPaneId, 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             secondTabId);

    const QString stableOverride = QStringLiteral("inactive override");
    workspace.confirmTitlePrompt(stablePromptId, stableOverride);
    QCOMPARE(firstPane->surfaceTitleOverride(),
             std::optional<QString>{stableOverride});
    QCOMPARE(workspace.currentTitle(), secondTitle);
    QTRY_COMPARE_WITH_TIMEOUT(requested.count(), 7, 1000);
    QCOMPARE(requested.at(6).at(1).toString(),
             QStringLiteral("Change Tab Title"));
    QCOMPARE(requested.at(6).at(2).toString(),
             QStringLiteral("stable base"));
    const quint64 tabPromptId = requested.at(6).at(0).toULongLong();
    workspace.cancelTitlePrompt(tabPromptId);

    // Removing the exact stable pane resolves its active prompt even when a
    // newly created sibling keeps the containing tab alive. Late OK is inert.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {firstTabId, firstPaneId, 0},
    }));
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QTRY_COMPARE_WITH_TIMEOUT(requested.count(), 8, 1000);
    const quint64 removedPromptId =
        requested.at(7).at(0).toULongLong();
    QPointer<TerminalPane> removedPane(firstPane);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ClosePane,
        {firstTabId, firstPaneId, 0},
    }));
    QTRY_VERIFY_WITH_TIMEOUT(removedPane.isNull(), 1000);
    QCOMPARE(resolved.count(), 8);
    workspace.confirmTitlePrompt(removedPromptId,
                                 QStringLiteral("stale"));
    QCOMPARE(workspace.currentTitle(), secondTitle);
}

void TerminalWorkspaceTest::broadSurfaceTitlePromptsShareFifoAndPruneRemovedPanes()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/broad-surface-title-prompt-XXXXXX")));
    QVERIFY(directory.isValid());
    const QString releasePath =
        directory.filePath(QStringLiteral("release"));

    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.workingDirectory = directory.path();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "while [ ! -e \"$1\" ]; do sleep 0.02; done; "
            "printf 'broad-selection-marker\\r\\n"
            "\\033]0;broad-ready\\007'; sleep 30"),
        QStringLiteral("broad-surface-prompt-test"),
        releasePath,
    };
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(900, 600);
    TerminalWorkspace workspace;
    workspace.setParentItem(window.contentItem());
    workspace.setSize(window.size());
    window.show();
    QTRY_COMPARE_WITH_TIMEOUT(workspace.window(), &window, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);

    const TabId firstTabId = workspace.tabModel()->idAt(0);
    const PaneId firstPaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    auto *firstController =
        firstPane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {firstTabId, firstPaneId, 0},
    }));
    const PaneId secondPaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane != firstPane) secondPane = pane;
    }
    QVERIFY(secondPane != nullptr);
    QVERIFY(secondPaneId != firstPaneId);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {firstTabId, secondPaneId, 0},
    }));
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);

    workspace.newTab();
    const TabId secondTabId =
        workspace.tabModel()->idAt(workspace.currentIndex());
    const PaneId thirdPaneId =
        workspace.tabModel()->entryAt(workspace.currentIndex())->activePaneId;
    TerminalPane *thirdPane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane != firstPane && pane != secondPane) thirdPane = pane;
    }
    QVERIFY(thirdPane != nullptr);
    QVERIFY(secondTabId != firstTabId);

    QFile release(releasePath);
    QVERIFY(release.open(QIODevice::WriteOnly));
    release.close();
    QTRY_COMPARE_WITH_TIMEOUT(firstController->title(),
                              QStringLiteral("broad-ready"), 3000);
    QVERIFY(firstPane->executeConfiguredAction(QStringLiteral("select_all")));
    QTRY_VERIFY_WITH_TIMEOUT(firstController->selectionAvailable(), 1000);
    firstPane->setSurfaceTitleOverride(
        std::optional<QString>{QStringLiteral("first seed")});
    secondPane->setSurfaceTitleOverride(
        std::optional<QString>{QStringLiteral("second override")});
    thirdPane->setSurfaceTitleOverride(
        std::optional<QString>{QStringLiteral("third override")});
    thirdPane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), thirdPane, 1000);

    const int selectedIndex = workspace.currentIndex();
    const PaneId selectedPaneId =
        workspace.tabModel()->entryAt(selectedIndex)->activePaneId;
    QSignalSpy requested(
        &workspace, &TerminalWorkspace::titlePromptRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::titlePromptResolved);
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("prompt_surface_title")));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.at(0).at(1).toString(),
             QStringLiteral("Change Terminal Title"));
    QCOMPARE(requested.at(0).at(2).toString(),
             QStringLiteral("first seed"));
    QCOMPARE(workspace.currentIndex(), selectedIndex);
    QCOMPARE(workspace.tabModel()->entryAt(selectedIndex)->activePaneId,
             selectedPaneId);
    QCOMPARE(window.activeFocusItem(), thirdPane);
    QVERIFY(firstController->selectionAvailable());

    const quint64 firstPromptId =
        requested.at(0).at(0).toULongLong();
    workspace.confirmTitlePrompt(firstPromptId,
                                 QStringLiteral("first override"));
    QCOMPARE(firstPane->surfaceTitleOverride(),
             std::optional<QString>{QStringLiteral("first override")});
    QTRY_COMPARE_WITH_TIMEOUT(requested.count(), 2, 1000);
    QCOMPARE(requested.at(1).at(2).toString(),
             QStringLiteral("second override"));
    workspace.cancelTitlePrompt(
        requested.at(1).at(0).toULongLong());
    QTRY_COMPARE_WITH_TIMEOUT(requested.count(), 3, 1000);
    QCOMPARE(requested.at(2).at(2).toString(),
             QStringLiteral("third override"));
    workspace.confirmTitlePrompt(
        requested.at(2).at(0).toULongLong(), QString{});
    QCOMPARE(resolved.count(), 3);
    QVERIFY(!thirdPane->surfaceTitleOverride().has_value());
    QCOMPARE(workspace.currentIndex(), selectedIndex);
    QCOMPARE(window.activeFocusItem(), thirdPane);
    QVERIFY(firstController->selectionAvailable());

    // A surface request followed by a tab request shares the same modal FIFO.
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QCOMPARE(requested.count(), 4);
    const quint64 mixedSurfacePromptId =
        requested.at(3).at(0).toULongLong();
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_tab_title")));
    QCOMPARE(requested.count(), 4);
    workspace.confirmTitlePrompt(mixedSurfacePromptId,
                                 QStringLiteral("mixed surface"));
    QTRY_COMPARE_WITH_TIMEOUT(requested.count(), 5, 1000);
    QCOMPARE(requested.at(4).at(1).toString(),
             QStringLiteral("Change Tab Title"));
    QCOMPARE(requested.at(4).at(2).toString(),
             QStringLiteral("🔍 second override"));
    workspace.cancelTitlePrompt(
        requested.at(4).at(0).toULongLong());

    // Add a fourth pane after the zoom assertion so removing its queued
    // request can exercise exact-pane pruning while the tab remains alive.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {firstTabId, secondPaneId, 0},
    }));
    const PaneId queuedPaneId =
        workspace.tabModel()
            ->entryAt(workspace.tabModel()->indexOf(firstTabId))
            ->activePaneId;
    TerminalPane *queuedPane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane != firstPane && pane != secondPane && pane != thirdPane) {
            queuedPane = pane;
        }
    }
    QVERIFY(queuedPane != nullptr);
    QVERIFY(queuedPaneId != firstPaneId);
    QVERIFY(queuedPaneId != secondPaneId);

    // Removing an active pane prompt advances to the next stable pane. Queued
    // requests for an exact removed pane and a later removed tab are pruned.
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QCOMPARE(requested.count(), 6);
    const quint64 removedPromptId =
        requested.at(5).at(0).toULongLong();
    QVERIFY(secondPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QVERIFY(queuedPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QVERIFY(thirdPane->executeConfiguredAction(
        QStringLiteral("prompt_surface_title")));
    QCOMPARE(requested.count(), 6);

    QPointer<TerminalPane> removedQueuedPane(queuedPane);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ClosePane,
        {firstTabId, queuedPaneId, 0},
    }));
    QTRY_VERIFY_WITH_TIMEOUT(removedQueuedPane.isNull(), 1000);
    QCOMPARE(requested.count(), 6);

    QPointer<TerminalPane> removedPane(firstPane);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ClosePane,
        {firstTabId, firstPaneId, 0},
    }));
    QTRY_VERIFY_WITH_TIMEOUT(removedPane.isNull(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(requested.count(), 7, 1000);
    QCOMPARE(requested.at(6).at(2).toString(),
             QStringLiteral("second override"));
    workspace.confirmTitlePrompt(removedPromptId,
                                 QStringLiteral("stale"));
    QCOMPARE(secondPane->surfaceTitleOverride(),
             std::optional<QString>{QStringLiteral("second override")});

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::CloseTab,
        {secondTabId, thirdPaneId, 0},
    }));
    QCOMPARE(workspace.tabCount(), 1);
    workspace.cancelTitlePrompt(
        requested.at(6).at(0).toULongLong());
    QCoreApplication::processEvents();
    QCOMPARE(requested.count(), 7);
    QCOMPARE(resolved.count(), 7);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId,
             secondPaneId);
}

void TerminalWorkspaceTest::tabTitlePromptsPreserveStableTargetsAndReset()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId firstTabId = workspace.tabModel()->idAt(0);
    const PaneId firstPaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);

    const auto entry = [&workspace](TabId id) {
        return workspace.tabModel()->entryAt(
            workspace.tabModel()->indexOf(id));
    };
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("set_tab_title:existing title")));
    QCOMPARE(entry(firstTabId)->titleOverride,
             QStringLiteral("existing title"));

    QSignalSpy requested(
        &workspace, &TerminalWorkspace::titlePromptRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::titlePromptResolved);
    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_tab_title")));
    QCOMPARE(requested.count(), 1);
    const quint64 firstPromptId =
        requested.at(0).at(0).toULongLong();
    QVERIFY(firstPromptId != 0);
    QCOMPARE(requested.at(0).at(1).toString(),
             QStringLiteral("Change Tab Title"));
    QCOMPARE(requested.at(0).at(2).toString(),
             QStringLiteral("existing title"));

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    const TabId secondTabId =
        workspace.tabModel()->idAt(workspace.currentIndex());
    const QString secondTitle = workspace.currentTitle();
    QVERIFY(secondTabId != firstTabId);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::MoveTab,
        {firstTabId, firstPaneId, 1},
    }));
    QCOMPARE(workspace.tabModel()->idAt(1), firstTabId);
    QCOMPARE(workspace.tabModel()->idAt(workspace.currentIndex()),
             secondTabId);

    const QString renamed = QStringLiteral("  renamed 👻  ");
    workspace.confirmTitlePrompt(firstPromptId, renamed);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(resolved.at(0).at(0).toULongLong(), firstPromptId);
    QCOMPARE(entry(firstTabId)->titleOverride, renamed);
    QCOMPARE(workspace.currentTitle(), secondTitle);

    // A late completion cannot mutate either the original target or the tab
    // currently occupying its former row.
    workspace.confirmTitlePrompt(firstPromptId, QStringLiteral("stale"));
    QCOMPARE(entry(firstTabId)->titleOverride, renamed);
    QVERIFY(entry(secondTabId)->titleOverride.isEmpty());

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_tab_title")));
    QCOMPARE(requested.count(), 2);
    const quint64 cancelPromptId =
        requested.at(1).at(0).toULongLong();
    QCOMPARE(requested.at(1).at(2).toString(), renamed);
    workspace.cancelTitlePrompt(cancelPromptId);
    QCOMPARE(resolved.count(), 2);
    QCOMPARE(entry(firstTabId)->titleOverride, renamed);

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_tab_title")));
    QCOMPARE(requested.count(), 3);
    const quint64 clearPromptId =
        requested.at(2).at(0).toULongLong();
    workspace.confirmTitlePrompt(clearPromptId, QString{});
    QCOMPARE(resolved.count(), 3);
    QVERIFY(entry(firstTabId)->titleOverride.isEmpty());

    QVERIFY(firstPane->executeConfiguredAction(
        QStringLiteral("prompt_tab_title")));
    QCOMPARE(requested.count(), 4);
    const quint64 basePromptId =
        requested.at(3).at(0).toULongLong();
    QCOMPARE(requested.at(3).at(2).toString(),
             entry(firstTabId)->title);
    workspace.cancelTitlePrompt(basePromptId);

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::PromptTabTitle,
        {secondTabId, firstPaneId, 0},
    }));
    QCOMPARE(requested.count(), 4);
}

void TerminalWorkspaceTest::broadTabTitlePromptsQueueEverySurfaceAndSurviveRemoval()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId firstTabId = workspace.tabModel()->idAt(0);
    const PaneId firstPaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {firstTabId, firstPaneId, 0},
    }));
    const PaneId secondPaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(secondPaneId.isValid());
    QVERIFY(secondPaneId != firstPaneId);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {firstTabId, secondPaneId, 0},
    }));

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    const TabId secondTabId =
        workspace.tabModel()->idAt(workspace.currentIndex());
    QVERIFY(secondTabId != firstTabId);
    const QString firstPromptInitial =
        QStringLiteral("🔍 ")
        + workspace.tabModel()
              ->entryAt(workspace.tabModel()->indexOf(firstTabId))->title;
    const QString secondPromptInitial =
        workspace.tabModel()
            ->entryAt(workspace.tabModel()->indexOf(secondTabId))->title;

    QSignalSpy requested(
        &workspace, &TerminalWorkspace::titlePromptRequested);
    QSignalSpy resolved(
        &workspace, &TerminalWorkspace::titlePromptResolved);
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("prompt_tab_title")));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.at(0).at(1).toString(),
             QStringLiteral("Change Tab Title"));
    QCOMPARE(requested.at(0).at(2).toString(), firstPromptInitial);
    const quint64 firstPromptId =
        requested.at(0).at(0).toULongLong();

    // Removing the originating split leaf does not cancel a prompt whose
    // stable containing tab still exists.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ClosePane,
        {firstTabId, firstPaneId, 0},
    }));
    QCOMPARE(workspace.tabCount(), 2);
    workspace.confirmTitlePrompt(firstPromptId,
                                 QStringLiteral("first tab"));
    QCOMPARE(workspace.tabModel()
                 ->entryAt(workspace.tabModel()->indexOf(firstTabId))
                 ->titleOverride,
             QStringLiteral("first tab"));

    // Pinned all/global dispatch invokes the action once per surface. The
    // second split's request retains the title snapshot captured before the
    // first prompt was accepted.
    QTRY_COMPARE_WITH_TIMEOUT(requested.count(), 2, 1000);
    QCOMPARE(requested.at(1).at(2).toString(), firstPromptInitial);
    const quint64 removedPromptId =
        requested.at(1).at(0).toULongLong();

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::CloseTab,
        {firstTabId, PaneId{}, 0},
    }));
    QCOMPARE(workspace.tabCount(), 1);
    QCOMPARE(resolved.count(), 2);

    // Removing the active target resolves it and advances to the queued
    // request for the next stable tab. A stale callback is harmless.
    QTRY_COMPARE_WITH_TIMEOUT(requested.count(), 3, 1000);
    QCOMPARE(requested.at(2).at(2).toString(), secondPromptInitial);
    const quint64 finalPromptId =
        requested.at(2).at(0).toULongLong();
    workspace.confirmTitlePrompt(removedPromptId,
                                 QStringLiteral("stale"));
    QVERIFY(workspace.tabModel()->entryAt(0)->titleOverride.isEmpty());

    workspace.confirmTitlePrompt(finalPromptId,
                                 QStringLiteral("second tab"));
    QCOMPARE(resolved.count(), 3);
    QCOMPARE(workspace.tabModel()->entryAt(0)->id, secondTabId);
    QCOMPARE(workspace.tabModel()->entryAt(0)->titleOverride,
             QStringLiteral("second tab"));
    QCoreApplication::processEvents();
    QCOMPARE(requested.count(), 3);
}

void TerminalWorkspaceTest::newTabPositionReloadsAndKeepsBroadOrder()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    {
        TerminalWorkspace workspace;
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        workspace.newTab();
        workspace.newTab();
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({TabId(1), TabId(2), TabId(3)}));
        const PaneId thirdPaneId =
            workspace.tabModel()->entryAt(2)->activePaneId;

        // The default current mode inserts after the selected first tab.
        workspace.setCurrentIndex(0);
        workspace.newTab();
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({
                     TabId(1), TabId(4), TabId(2), TabId(3),
                 }));
        QCOMPARE(workspace.currentIndex(), 1);

        LaunchOptions reloadedOptions = options;
        reloadedOptions.windowNewTabPosition = WindowNewTabPosition::End;
        workspace.applyLaunchOptions(reloadedOptions);
        const QVector<TabId> beforeEndInsert = tabIds(workspace);

        // Placement follows the selected tab policy, independently of the
        // explicit inactive pane retained as the inheritance source.
        QVERIFY(workspace.dispatchAction({
            WorkspaceAction::NewTab,
            {TabId(3), thirdPaneId, 0},
        }));
        QCOMPARE(beforeEndInsert,
                 QVector<TabId>({
                     TabId(1), TabId(4), TabId(2), TabId(3),
                 }));
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({
                     TabId(1), TabId(4), TabId(2), TabId(3), TabId(5),
                 }));
        QCOMPARE(workspace.currentIndex(), 4);

        reloadedOptions.windowNewTabPosition = WindowNewTabPosition::Current;
        workspace.applyLaunchOptions(reloadedOptions);
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({
                     TabId(1), TabId(4), TabId(2), TabId(3), TabId(5),
                 }));
        workspace.setCurrentIndex(1);
        QVERIFY(workspace.dispatchAction({
            WorkspaceAction::NewTab,
            {TabId(3), thirdPaneId, 0},
        }));
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({
                     TabId(1), TabId(4), TabId(6), TabId(2), TabId(3),
                     TabId(5),
                 }));
        QCOMPARE(workspace.currentIndex(), 2);
    }

    {
        TerminalWorkspace workspace;
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        workspace.newTab();
        workspace.newTab();
        workspace.setCurrentIndex(1);

        // Broad fanout retains the three original pane sources. Each new tab
        // becomes selected, so current mode inserts the next one immediately
        // after it and leaves one contiguous block in snapshot order.
        QVERIFY(workspace.executeSurfaceActionOnAllPanes(
            QStringLiteral("new_tab")));
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({
                     TabId(1), TabId(2), TabId(4), TabId(5), TabId(6),
                     TabId(3),
                 }));
        QCOMPARE(workspace.currentIndex(), 4);
    }

    {
        TerminalWorkspace workspace;
        QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
        workspace.newTab();
        workspace.newTab();
        workspace.setCurrentIndex(1);

        LaunchOptions reloadedOptions = options;
        reloadedOptions.windowNewTabPosition = WindowNewTabPosition::End;
        workspace.applyLaunchOptions(reloadedOptions);

        // End mode appends the stable broad source snapshot in order even
        // though every creation advances the selected tab.
        QVERIFY(workspace.executeSurfaceActionOnAllPanes(
            QStringLiteral("new_tab")));
        QCOMPARE(tabIds(workspace),
                 QVector<TabId>({
                     TabId(1), TabId(2), TabId(3), TabId(4), TabId(5),
                     TabId(6),
                 }));
        QCOMPARE(workspace.currentIndex(), 5);
    }
}

void TerminalWorkspaceTest::tabBarVisibilityTracksPolicyAndCount()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.confirmCloseMode = ConfirmCloseMode::Never;
    options.windowShowTabBar = WindowShowTabBar::Auto;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    QVERIFY(!workspace.tabBarVisible());
    QSignalSpy visibilityChanged(
        &workspace, &TerminalWorkspace::tabBarVisibleChanged);

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    QVERIFY(workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 1);

    workspace.closeCurrentTab();
    QCOMPARE(workspace.tabCount(), 1);
    QVERIFY(!workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 2);

    LaunchOptions reloadedOptions = options;
    reloadedOptions.windowShowTabBar = WindowShowTabBar::Always;
    workspace.applyLaunchOptions(reloadedOptions);
    QVERIFY(workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 3);

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    QVERIFY(workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 3);

    reloadedOptions.windowShowTabBar = WindowShowTabBar::Never;
    workspace.applyLaunchOptions(reloadedOptions);
    QVERIFY(!workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 4);

    workspace.closeCurrentTab();
    QCOMPARE(workspace.tabCount(), 1);
    QVERIFY(!workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 4);

    reloadedOptions.windowShowTabBar = WindowShowTabBar::Auto;
    workspace.applyLaunchOptions(reloadedOptions);
    QVERIFY(!workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 4);

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 2);
    QVERIFY(workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 5);

    reloadedOptions.confirmCloseMode = ConfirmCloseMode::Always;
    workspace.applyLaunchOptions(reloadedOptions);
    QCOMPARE(visibilityChanged.count(), 5);
    QSignalSpy confirmation(
        &workspace, &TerminalWorkspace::closeConfirmationRequested);
    workspace.closeCurrentTab();
    QCOMPARE(workspace.tabCount(), 2);
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(visibilityChanged.count(), 5);

    // Resolving a pending close during reload must publish the count-driven
    // auto transition exactly once; the reload path must not duplicate it.
    reloadedOptions.confirmCloseMode = ConfirmCloseMode::Never;
    workspace.applyLaunchOptions(reloadedOptions);
    QCOMPARE(workspace.tabCount(), 1);
    QVERIFY(!workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 6);

    reloadedOptions.windowShowTabBar = WindowShowTabBar::Always;
    workspace.applyLaunchOptions(reloadedOptions);
    QVERIFY(workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 7);

    QSignalSpy quit(&workspace, &TerminalWorkspace::windowCloseApproved);
    workspace.closeCurrentTab();
    QCOMPARE(workspace.tabCount(), 0);
    QVERIFY(workspace.tabBarVisible());
    QCOMPARE(visibilityChanged.count(), 7);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 1000);
}

void TerminalWorkspaceTest::tabsLocationReloadsAsPresentationPolicy()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.tabsLocation = TabsLocation::Top;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    TerminalPane *const pane = workspace.findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    const int initialIndex = workspace.currentIndex();
    QSignalSpy locationChanged(&workspace,
                               &TerminalWorkspace::tabsLocationChanged);

    QVERIFY(!workspace.tabBarAtBottom());
    workspace.applyLaunchOptions(options);
    QCOMPARE(locationChanged.count(), 0);

    LaunchOptions reloaded = options;
    reloaded.tabsLocation = TabsLocation::Bottom;
    workspace.applyLaunchOptions(reloaded);
    QVERIFY(workspace.tabBarAtBottom());
    QCOMPARE(locationChanged.count(), 1);
    QCOMPARE(workspace.findChild<TerminalPane *>(), pane);
    QCOMPARE(workspace.tabCount(), 1);
    QCOMPARE(workspace.currentIndex(), initialIndex);

    workspace.applyLaunchOptions(reloaded);
    QCOMPARE(locationChanged.count(), 1);

    reloaded.tabsLocation = TabsLocation::Top;
    workspace.applyLaunchOptions(reloaded);
    QVERIFY(!workspace.tabBarAtBottom());
    QCOMPARE(locationChanged.count(), 2);
    QCOMPARE(workspace.findChild<TerminalPane *>(), pane);
}

void TerminalWorkspaceTest::splitNavigationWrapsInTreeAndSpatialOrder()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId first = workspace.tabModel()->entryAt(0)->activePaneId;

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, first, 0},
    }));
    const PaneId second = workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(second.isValid());
    QVERIFY(second != first);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitDown,
        {tabId, second, 0},
    }));
    const PaneId third = workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(third.isValid());
    QVERIFY(third != first && third != second);

    // DFS leaf order is first, second, third; relative navigation wraps.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, third, 1},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, first);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, first, -1},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, third);
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, third, 3},
    }));

    // The first pane has no pane to its left. Spatial navigation wraps to the
    // nearest pane on the opposite edge, which is the upper-right pane.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePane,
        {tabId, first, Qt::Key_Left},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, second);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePane,
        {tabId, second, Qt::Key_Right},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, first);

    // Likewise, wrapping upward from the full-height left pane selects the
    // lower-right pane because it is nearest after translating by one grid.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePane,
        {tabId, first, Qt::Key_Up},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, third);
}

void TerminalWorkspaceTest::relativeSplitNavigationUsesExplicitSourceAndTreeOrder()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId first = workspace.tabModel()->entryAt(0)->activePaneId;

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, first, 1},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, first);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, first, 0},
    }));
    const PaneId second = workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitLeft,
        {tabId, second, 0},
    }));
    const PaneId third = workspace.tabModel()->entryAt(0)->activePaneId;

    // The split tree is now [first, third, second], unlike creation order.
    // Navigating from an explicit inactive source must use that tree order.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {tabId, second, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, first, 1},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, third);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, first, -1},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, second);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, second, 1},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, first);

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, PaneId(999'999), 1},
    }));
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {TabId(999'999), first, 1},
    }));
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, first);
}

void TerminalWorkspaceTest::splitResizeAndEqualizeRespectTreeAxes()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId firstId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    const PaneId secondId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane) secondPane = candidate;
    }
    QVERIFY(secondPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, secondId, 0},
    }));
    const PaneId thirdId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *thirdPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane && candidate != secondPane) {
            thirdPane = candidate;
        }
    }
    QVERIFY(thirdPane != nullptr);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 3);

    const qreal initialSecondWidth = secondPane->width();
    const qreal initialThirdWidth = thirdPane->width();
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ResizeSplit,
        {tabId, thirdId, Qt::Key_Right, 60},
    }));
    QVERIFY(qAbs(secondPane->width() - (initialSecondWidth + 60.0)) <= 1.0);
    QVERIFY(qAbs(thirdPane->width() - (initialThirdWidth - 60.0)) <= 1.0);

    // A split tree is performable even when this leaf has no ancestor on the
    // requested axis, but the tree remains unchanged. Zero amount is not
    // performable.
    const QSizeF unchangedFirst = firstPane->size();
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ResizeSplit,
        {tabId, firstId, Qt::Key_Up, 10},
    }));
    QCOMPARE(firstPane->size(), unchangedFirst);
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ResizeSplit,
        {tabId, firstId, Qt::Key_Right, 0},
    }));

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitDown,
        {tabId, thirdId, 0},
    }));
    const PaneId fourthId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *fourthPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane && candidate != secondPane
            && candidate != thirdPane) {
            fourthPane = candidate;
        }
    }
    QVERIFY(fourthPane != nullptr);

    const qreal initialThirdHeight = thirdPane->height();
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ResizeSplit,
        {tabId, fourthId, Qt::Key_Down, 60},
    }));
    QVERIFY(qAbs(thirdPane->height() - (initialThirdHeight + 60.0)) <= 1.0);

    // Add a second vertical level so equalization must propagate the nested
    // subtree's vertical weight through its parent.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitDown,
        {tabId, fourthId, 0},
    }));
    const PaneId fifthId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *fifthPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane && candidate != secondPane
            && candidate != thirdPane && candidate != fourthPane) {
            fifthPane = candidate;
        }
    }
    QVERIFY(fifthPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::EqualizeSplits,
        {tabId, fifthId, 0},
    }));
    // The two contiguous horizontal splits weight the left leaf against the
    // two right-hand units at 1:2, while the perpendicular vertical subtree
    // counts as one unit. All three columns therefore have equal width.
    QVERIFY(qAbs(firstPane->width() - secondPane->width()) <= 1.0);
    QVERIFY(qAbs(secondPane->width() - thirdPane->width()) <= 1.0);
    QVERIFY(qAbs(thirdPane->height() - fourthPane->height()) <= 1.0);
    QVERIFY(qAbs(fourthPane->height() - fifthPane->height()) <= 1.0);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, fifthId);
}

void TerminalWorkspaceTest::dragsExactNestedSplitDividerAndPreservesFocus()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(902, 602);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
    if (qEnvironmentVariableIsSet("GHOSTTY_QT_EXPECT_HIDPI")) {
        QVERIFY(window.devicePixelRatio() >= 2.0);
    }
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);

    const TabId tabId = workspace->tabModel()->idAt(0);
    const PaneId firstId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace->findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);

    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    const PaneId secondId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane) secondPane = pane;
    }
    QVERIFY(secondPane != nullptr);

    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, secondId, 0},
    }));
    const PaneId thirdId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *thirdPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane && pane != secondPane) thirdPane = pane;
    }
    QVERIFY(thirdPane != nullptr);
    QCOMPARE(splitDividerItems(workspace.get()).size(), 2);

    thirdPane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), thirdPane, 1000);
    const QSizeF firstSize = firstPane->size();
    const qreal secondWidth = secondPane->width();
    const qreal thirdWidth = thirdPane->width();
    const qreal nestedEdge = secondPane->x() + secondPane->width();
    QCOMPARE(thirdPane->x(), nestedEdge + 2.0);

    QQuickItem *nestedDivider = nullptr;
    for (QQuickItem *divider : splitDividerItems(workspace.get())) {
        QCOMPARE(divider->width(), 2.0);
        QCOMPARE(divider->cursor().shape(), Qt::SplitHCursor);
        if (qAbs(divider->x() - nestedEdge) <= 0.01) {
            nestedDivider = divider;
        }
    }
    QVERIFY(nestedDivider != nullptr);

    QSignalSpy firstActivated(firstPane, &TerminalPane::activated);
    QSignalSpy secondActivated(secondPane, &TerminalPane::activated);
    QSignalSpy thirdActivated(thirdPane, &TerminalPane::activated);
    const QPoint horizontalStart = windowPoint(
        workspace.get(), QPointF(nestedEdge + 1.0,
                                 workspace->height() / 2.0));
    const QPoint horizontalEnd = horizontalStart + QPoint(60, 0);
    QTest::mouseMove(&window, horizontalStart);
    QTRY_COMPARE_WITH_TIMEOUT(window.cursor().shape(), Qt::SplitHCursor, 1000);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier,
                      horizontalStart);
    QVERIFY(window.mouseGrabberItem() != nullptr);
    QCOMPARE(window.mouseGrabberItem()->objectName(),
             QStringLiteral("_ghosttyQtSplitDivider"));
    QTest::mouseMove(&window, horizontalEnd);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                        horizontalEnd);
    QVERIFY(window.mouseGrabberItem() == nullptr);

    QCOMPARE(firstPane->size(), firstSize);
    QVERIFY(qAbs(secondPane->width() - (secondWidth + 60.0)) <= 1.0);
    QVERIFY(qAbs(thirdPane->width() - (thirdWidth - 60.0)) <= 1.0);
    QCOMPARE(workspace->tabModel()->entryAt(0)->activePaneId, thirdId);
    QCOMPARE(window.activeFocusItem(), thirdPane);
    QCOMPARE(firstActivated.count(), 0);
    QCOMPARE(secondActivated.count(), 0);
    QCOMPARE(thirdActivated.count(), 0);

    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitDown,
        {tabId, thirdId, 0},
    }));
    const PaneId fourthId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *fourthPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane && pane != secondPane && pane != thirdPane) {
            fourthPane = pane;
        }
    }
    QVERIFY(fourthPane != nullptr);
    QCOMPARE(splitDividerItems(workspace.get()).size(), 3);
    fourthPane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), fourthPane, 1000);

    const QSizeF firstBeforeVertical = firstPane->size();
    const QSizeF secondBeforeVertical = secondPane->size();
    const qreal thirdHeight = thirdPane->height();
    const qreal fourthHeight = fourthPane->height();
    const qreal verticalEdge = thirdPane->y() + thirdPane->height();
    QCOMPARE(fourthPane->y(), verticalEdge + 2.0);
    QQuickItem *verticalDivider = nullptr;
    for (QQuickItem *divider : splitDividerItems(workspace.get())) {
        QCOMPARE(divider->z(), 1.0);
        if (divider->cursor().shape() == Qt::SplitVCursor) {
            verticalDivider = divider;
        }
    }
    QVERIFY(verticalDivider != nullptr);
    QCOMPARE(verticalDivider->position(),
             QPointF(thirdPane->x(), verticalEdge));
    QCOMPARE(verticalDivider->size(), QSizeF(thirdPane->width(), 2.0));

    // Exact half-open gap geometry resolves the T-junction without relying
    // on QObject creation or stacking order: the nested horizontal handle
    // ends where the vertical handle's subtree begins.
    const QPoint horizontalSide = windowPoint(
        workspace.get(), QPointF(thirdPane->x() - 1.0,
                                 verticalEdge + 1.0));
    const QPoint verticalSide = windowPoint(
        workspace.get(), QPointF(thirdPane->x(), verticalEdge + 1.0));
    QTest::mouseMove(&window, horizontalSide);
    QTRY_COMPARE_WITH_TIMEOUT(window.cursor().shape(), Qt::SplitHCursor, 1000);
    QTest::mouseMove(&window, verticalSide);
    QTRY_COMPARE_WITH_TIMEOUT(window.cursor().shape(), Qt::SplitVCursor, 1000);

    const QPoint verticalStart = windowPoint(
        workspace.get(), QPointF(thirdPane->x() + thirdPane->width() / 2.0,
                                 verticalEdge + 1.0));
    const QPoint verticalEnd = verticalStart + QPoint(0, 60);
    QTest::mouseMove(&window, verticalStart);
    QTRY_COMPARE_WITH_TIMEOUT(window.cursor().shape(), Qt::SplitVCursor, 1000);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, verticalStart);
    QVERIFY(window.mouseGrabberItem() != nullptr);
    QCOMPARE(window.mouseGrabberItem()->objectName(),
             QStringLiteral("_ghosttyQtSplitDivider"));
    QTest::mouseMove(&window, verticalEnd);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, verticalEnd);

    QCOMPARE(firstPane->size(), firstBeforeVertical);
    QCOMPARE(secondPane->size(), secondBeforeVertical);
    QVERIFY(qAbs(thirdPane->height() - (thirdHeight + 60.0)) <= 1.0);
    QVERIFY(qAbs(fourthPane->height() - (fourthHeight - 60.0)) <= 1.0);
    QCOMPARE(workspace->tabModel()->entryAt(0)->activePaneId, fourthId);
    QCOMPARE(window.activeFocusItem(), fourthPane);

    workspace.reset();
    window.close();
}

void TerminalWorkspaceTest::splitDividerColorReloadsWithoutRelayout()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    const QColor defaultColor(QStringLiteral("#3b4252"));
    const QColor firstColor(QStringLiteral("#a1b2c3"));
    const QColor secondColor(QStringLiteral("#d4a017"));
    QQuickWindow window;
    window.setColor(Qt::transparent);
    window.resize(604, 404);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
    if (qEnvironmentVariableIsSet("GHOSTTY_QT_EXPECT_HIDPI")) {
        QVERIFY(window.devicePixelRatio() >= 2.0);
    }
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);

    const TabId tabId = workspace->tabModel()->idAt(0);
    const PaneId firstId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace->findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    const PaneId secondId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane) {
            secondPane = pane;
        }
    }
    QVERIFY(secondPane != nullptr);
    secondPane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), secondPane, 1000);

    QList<QQuickItem *> dividers = splitDividerItems(workspace.get());
    QCOMPARE(dividers.size(), 1);
    QQuickItem *rootDivider = dividers.constFirst();
    QCOMPARE(rootDivider->width(), 2.0);
    QTRY_VERIFY_WITH_TIMEOUT(
        dividerPaintsExactColor(&window, rootDivider, defaultColor, false),
        2000);

    const QRectF rootDividerGeometry(rootDivider->position(),
                                     rootDivider->size());
    const QRectF firstPaneGeometry(firstPane->position(), firstPane->size());
    const QRectF secondPaneGeometry(secondPane->position(), secondPane->size());
    QSignalSpy secondActivated(secondPane, &TerminalPane::activated);

    LaunchOptions reloadedOptions = options;
    reloadedOptions.splitAppearance.dividerColor = firstColor;
    workspace->applyLaunchOptions(reloadedOptions);

    dividers = splitDividerItems(workspace.get());
    QCOMPARE(dividers.size(), 1);
    QCOMPARE(dividers.constFirst(), rootDivider);
    QCOMPARE(QRectF(rootDivider->position(), rootDivider->size()),
             rootDividerGeometry);
    QCOMPARE(QRectF(firstPane->position(), firstPane->size()),
             firstPaneGeometry);
    QCOMPARE(QRectF(secondPane->position(), secondPane->size()),
             secondPaneGeometry);
    QCOMPARE(workspace->tabModel()->entryAt(0)->activePaneId, secondId);
    QCOMPARE(window.activeFocusItem(), secondPane);
    QCOMPARE(secondActivated.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        dividerPaintsExactColor(&window, rootDivider, firstColor),
        2000);

    // A divider created after reload inherits the same workspace-owned color.
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitDown,
        {tabId, secondId, 0},
    }));
    const PaneId thirdId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *thirdPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane && pane != secondPane) {
            thirdPane = pane;
        }
    }
    QVERIFY(thirdPane != nullptr);
    thirdPane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), thirdPane, 1000);
    dividers = splitDividerItems(workspace.get());
    QCOMPARE(dividers.size(), 2);
    for (QQuickItem *divider : std::as_const(dividers)) {
        QTRY_VERIFY_WITH_TIMEOUT(
            dividerPaintsExactColor(&window, divider, firstColor),
            2000);
    }

    struct DividerState {
        QQuickItem *item = nullptr;
        QRectF geometry;
        Qt::CursorShape cursor = Qt::ArrowCursor;
    };
    std::vector<DividerState> dividerStates;
    dividerStates.reserve(static_cast<std::size_t>(dividers.size()));
    for (QQuickItem *divider : std::as_const(dividers)) {
        dividerStates.push_back({
            .item = divider,
            .geometry = {divider->position(), divider->size()},
            .cursor = divider->cursor().shape(),
        });
    }
    const QList<TerminalPane *> panes =
        workspace->findChildren<TerminalPane *>();
    std::vector<std::pair<TerminalPane *, QRectF>> paneStates;
    paneStates.reserve(static_cast<std::size_t>(panes.size()));
    for (TerminalPane *pane : panes) {
        paneStates.emplace_back(
            pane, QRectF(pane->position(), pane->size()));
    }
    QSignalSpy thirdActivated(thirdPane, &TerminalPane::activated);

    reloadedOptions.splitAppearance.dividerColor = secondColor;
    workspace->applyLaunchOptions(reloadedOptions);
    for (const DividerState &state : dividerStates) {
        QVERIFY(splitDividerItems(workspace.get()).contains(state.item));
        QCOMPARE(QRectF(state.item->position(), state.item->size()),
                 state.geometry);
        QCOMPARE(state.item->cursor().shape(), state.cursor);
        QCOMPARE(state.item->z(), 1.0);
        QCOMPARE(state.item->acceptedMouseButtons(), Qt::LeftButton);
        QCOMPARE(state.item->focusPolicy(), Qt::NoFocus);
        QTRY_VERIFY_WITH_TIMEOUT(
            dividerPaintsExactColor(&window, state.item, secondColor),
            2000);
    }
    for (const auto &[pane, geometry] : paneStates) {
        QCOMPARE(QRectF(pane->position(), pane->size()), geometry);
    }
    QCOMPARE(workspace->tabModel()->entryAt(0)->activePaneId, thirdId);
    QCOMPARE(window.activeFocusItem(), thirdPane);
    QCOMPARE(thirdActivated.count(), 0);

    // Empty canonical output restores the frontend's opaque ordinary gap
    // color without recreating the handles, even over a transparent host.
    reloadedOptions.splitAppearance.dividerColor.reset();
    workspace->applyLaunchOptions(reloadedOptions);
    for (const DividerState &state : dividerStates) {
        QVERIFY(splitDividerItems(workspace.get()).contains(state.item));
        QCOMPARE(QRectF(state.item->position(), state.item->size()),
                 state.geometry);
        QTRY_VERIFY_WITH_TIMEOUT(
            dividerPaintsExactColor(
                &window, state.item, defaultColor, false),
            2000);
    }
    QCOMPARE(window.activeFocusItem(), thirdPane);
    QCOMPARE(thirdActivated.count(), 0);

    // Zoom destroys the handles. Unzoom recreates them with the newest
    // effective color rather than a stale construction-time value.
    reloadedOptions.splitAppearance.dividerColor = secondColor;
    workspace->applyLaunchOptions(reloadedOptions);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, thirdId, 0},
    }));
    QVERIFY(splitDividerItems(workspace.get()).isEmpty());
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, thirdId, 0},
    }));
    dividers = splitDividerItems(workspace.get());
    QCOMPARE(dividers.size(), 2);
    for (QQuickItem *divider : std::as_const(dividers)) {
        QTRY_VERIFY_WITH_TIMEOUT(
            dividerPaintsExactColor(&window, divider, secondColor),
            2000);
    }
    QCOMPARE(workspace->tabModel()->entryAt(0)->activePaneId, thirdId);
    QCOMPARE(window.activeFocusItem(), thirdPane);

    workspace->newTab();
    QCOMPARE(workspace->currentIndex(), 1);
    QVERIFY(splitDividerItems(workspace.get()).isEmpty());
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ActivateTab,
        {tabId, PaneId{}, 0},
    }));
    dividers = splitDividerItems(workspace.get());
    QCOMPARE(dividers.size(), 2);
    for (QQuickItem *divider : std::as_const(dividers)) {
        QTRY_VERIFY_WITH_TIMEOUT(
            dividerPaintsExactColor(&window, divider, secondColor),
            2000);
    }

    QQuickWindow secondWindow;
    secondWindow.setColor(QColor(QStringLiteral("#24160b")));
    secondWindow.resize(window.size());
    secondWindow.show();
    QTRY_VERIFY_WITH_TIMEOUT(secondWindow.isExposed(), 1000);
    workspace->setParentItem(secondWindow.contentItem());
    workspace->setSize(secondWindow.size());
    dividers = splitDividerItems(workspace.get());
    QCOMPARE(dividers.size(), 2);
    for (QQuickItem *divider : std::as_const(dividers)) {
        QTRY_VERIFY_WITH_TIMEOUT(
            dividerPaintsExactColor(&secondWindow, divider, secondColor),
            2000);
    }

    workspace.reset();
    secondWindow.close();
    window.close();
}

void TerminalWorkspaceTest::dimsUnfocusedSplitPanesAcrossLifecycle()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    const QColor background(QStringLiteral("#204060"));
    const QColor firstFill(QStringLiteral("#c04080"));
    const QColor secondFill(QStringLiteral("#20c080"));
    const QColor dividerColor(QStringLiteral("#00ff00"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = background;
    options.splitAppearance = {
        .unfocusedOpacity = 0.5,
        .unfocusedFill = firstFill,
        .dividerColor = dividerColor,
    };
    TerminalWorkspace::setDefaultLaunchOptions(options);

    const QColor windowColor(QStringLiteral("#07111b"));
    QQuickWindow window;
    window.setColor(windowColor);
    window.resize(604, 404);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    auto *focusSink = new QQuickItem(window.contentItem());
    focusSink->setSize(QSizeF(1.0, 1.0));
    focusSink->setPosition(QPointF(window.width() - 1.0,
                                   window.height() - 1.0));
    focusSink->setFocusPolicy(Qt::StrongFocus);
    focusSink->setZ(2.0);

    window.show();
    window.requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 1000);
    if (qEnvironmentVariableIsSet("GHOSTTY_QT_EXPECT_HIDPI")) {
        QVERIFY(window.devicePixelRatio() >= 2.0);
    }
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);

    const TabId splitTabId = workspace->tabModel()->idAt(0);
    const PaneId firstId =
        workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace->findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    auto *firstController = firstPane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);

    // A single leaf never dims, even when another frontend item owns focus.
    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(terminalPaneRenderProbe(firstPane)
                .unfocusedSplitOverlayRect.isEmpty());

    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitRight,
        {splitTabId, firstId, 0},
    }));
    const PaneId secondId =
        workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane) {
            secondPane = pane;
        }
    }
    QVERIFY(secondPane != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), secondPane, 1000);

    const QImage firstSplitImage = window.grabWindow();
    QVERIFY(!firstSplitImage.isNull());
    const TerminalPaneRenderProbeSnapshot firstDimmed =
        terminalPaneRenderProbe(firstPane);
    const TerminalPaneRenderProbeSnapshot secondFocused =
        terminalPaneRenderProbe(secondPane);
    QCOMPARE(firstDimmed.unfocusedSplitOverlayRect,
             firstPane->boundingRect());
    QVERIFY(secondFocused.unfocusedSplitOverlayRect.isEmpty());
    QColor firstOverlay = firstFill;
    firstOverlay.setAlphaF(0.5);
    QCOMPARE(firstDimmed.unfocusedSplitOverlayColor, firstOverlay);
    const qreal quietY = firstPane->height() / 2.0;
    const QPointF firstLeft(20.0, quietY);
    const QPointF secondLeft(1.0, quietY);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        const QImage image = window.grabWindow();
        if (image.isNull()) {
            return false;
        }
        const QColor activePixel = itemPixel(
            window, *secondPane, image, secondLeft);
        const QColor expectedDimmed = sourceOver(
            activePixel, firstFill, 0.5);
        const qreal xScale = static_cast<qreal>(image.width())
            / window.width();
        const qreal yScale = static_cast<qreal>(image.height())
            / window.height();
        const qreal firstRightScene = firstPane->mapToScene(
            QPointF(firstPane->width(), quietY)).x();
        const qreal secondLeftScene = secondPane->mapToScene(
            QPointF(0.0, quietY)).x();
        const qreal sampleYScene = firstPane->mapToScene(
            QPointF(0.0, quietY)).y();
        const int firstLastPixel = std::clamp(
            static_cast<int>(std::ceil(firstRightScene * xScale)) - 1,
            0, image.width() - 1);
        const int secondFirstPixel = std::clamp(
            static_cast<int>(std::floor(secondLeftScene * xScale)),
            0, image.width() - 1);
        const int sampleY = std::clamp(
            static_cast<int>(std::floor(sampleYScene * yScale)),
            0, image.height() - 1);
        return approximatelyEqual(
                itemPixel(window, *firstPane, image, firstLeft),
                expectedDimmed)
            && approximatelyEqual(
                image.pixelColor(firstLastPixel, sampleY), expectedDimmed)
            && approximatelyEqual(
                image.pixelColor(secondFirstPixel, sampleY), activePixel);
    }(), 2000);
    const QList<QQuickItem *> initialDividers =
        splitDividerItems(workspace.get());
    QCOMPARE(initialDividers.size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        dividerPaintsExactColor(
            &window, initialDividers.constFirst(), dividerColor),
        2000);

    const QRectF firstGeometry(firstPane->position(), firstPane->size());
    const QRectF secondGeometry(secondPane->position(), secondPane->size());
    const QRectF dividerGeometry(initialDividers.constFirst()->position(),
                                 initialDividers.constFirst()->size());

    firstPane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), firstPane, 1000);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(terminalPaneRenderProbe(firstPane)
                .unfocusedSplitOverlayRect.isEmpty());
    QCOMPARE(terminalPaneRenderProbe(secondPane)
                 .unfocusedSplitOverlayRect,
             secondPane->boundingRect());

    // Logical active-pane identity is deliberately insufficient: moving
    // actual focus outside the terminal dims every visible split pane.
    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);
    QVERIFY(!window.grabWindow().isNull());
    QCOMPARE(terminalPaneRenderProbe(firstPane)
                 .unfocusedSplitOverlayRect,
             firstPane->boundingRect());
    QCOMPARE(terminalPaneRenderProbe(secondPane)
                 .unfocusedSplitOverlayRect,
             secondPane->boundingRect());

    // Search suppression is pane-local and applies even with an empty query.
    QVERIFY(firstPane->executeConfiguredAction(QStringLiteral("start_search")));
    QVERIFY(firstPane->searchUiActive());
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(terminalPaneRenderProbe(firstPane)
                .unfocusedSplitOverlayRect.isEmpty());
    QCOMPARE(terminalPaneRenderProbe(secondPane)
                 .unfocusedSplitOverlayRect,
             secondPane->boundingRect());
    firstPane->endSearchUi();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), firstPane, 1000);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(terminalPaneRenderProbe(firstPane)
                .unfocusedSplitOverlayRect.isEmpty());
    QCOMPARE(terminalPaneRenderProbe(secondPane)
                 .unfocusedSplitOverlayRect,
             secondPane->boundingRect());

    const TerminalPaneRenderProbeSnapshot beforeReload =
        terminalPaneRenderProbe(secondPane);
    QSignalSpy firstActivated(firstPane, &TerminalPane::activated);
    LaunchOptions reloadedOptions = options;
    reloadedOptions.splitAppearance.unfocusedOpacity = 0.2;
    reloadedOptions.splitAppearance.unfocusedFill = secondFill;
    workspace->applyLaunchOptions(reloadedOptions);
    const QImage reloadedImage = window.grabWindow();
    QVERIFY(!reloadedImage.isNull());
    const TerminalPaneRenderProbeSnapshot afterReload =
        terminalPaneRenderProbe(secondPane);
    QCOMPARE(afterReload.rootSerial, beforeReload.rootSerial);
    QCOMPARE(afterReload.unfocusedSplitOverlaySerial,
             beforeReload.unfocusedSplitOverlaySerial);
    QCOMPARE(afterReload.rowNodeSerials, beforeReload.rowNodeSerials);
    QCOMPARE(afterReload.rowBuildCounts, beforeReload.rowBuildCounts);
    QColor secondOverlay = secondFill;
    secondOverlay.setAlphaF(0.8);
    QCOMPARE(afterReload.unfocusedSplitOverlayColor, secondOverlay);
    QCOMPARE(QRectF(firstPane->position(), firstPane->size()), firstGeometry);
    QCOMPARE(QRectF(secondPane->position(), secondPane->size()), secondGeometry);
    QCOMPARE(QRectF(initialDividers.constFirst()->position(),
                    initialDividers.constFirst()->size()),
             dividerGeometry);
    QCOMPARE(window.activeFocusItem(), firstPane);
    QCOMPARE(firstActivated.count(), 0);
    QCOMPARE(firstPane->findChild<TerminalController *>(), firstController);

    // A pane created after reload inherits the newest frontend appearance.
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitDown,
        {splitTabId, firstId, 0},
    }));
    const PaneId thirdId =
        workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *thirdPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane && pane != secondPane) {
            thirdPane = pane;
        }
    }
    QVERIFY(thirdPane != nullptr);
    QPointer<TerminalPane> thirdGuard(thirdPane);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), thirdPane, 1000);
    QVERIFY(!window.grabWindow().isNull());
    for (TerminalPane *pane : {firstPane, secondPane}) {
        QCOMPARE(terminalPaneRenderProbe(pane)
                     .unfocusedSplitOverlayColor,
                 secondOverlay);
    }
    QVERIFY(terminalPaneRenderProbe(thirdPane)
                .unfocusedSplitOverlayRect.isEmpty());

    firstPane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), firstPane, 1000);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot futurePaneDimmed =
        terminalPaneRenderProbe(thirdPane);
    QCOMPARE(futurePaneDimmed.unfocusedSplitOverlayRect,
             thirdPane->boundingRect());
    QCOMPARE(futurePaneDimmed.unfocusedSplitOverlayColor, secondOverlay);

    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ClosePane,
        {splitTabId, thirdId, 0},
    }));
    QTRY_VERIFY_WITH_TIMEOUT(thirdGuard.isNull(), 2000);
    QCOMPARE(workspace->findChildren<TerminalPane *>().size(), 2);

    // An unsplit tab remains clear; returning restores the split predicate.
    workspace->newTab();
    QCOMPARE(workspace->currentIndex(), 1);
    TerminalPane *singleTabPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane && pane != secondPane) {
            singleTabPane = pane;
        }
    }
    QVERIFY(singleTabPane != nullptr);
    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(terminalPaneRenderProbe(singleTabPane)
                .unfocusedSplitOverlayRect.isEmpty());
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ActivateTab,
        {splitTabId, PaneId{}, 0},
    }));
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), firstPane, 1000);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(terminalPaneRenderProbe(firstPane)
                .unfocusedSplitOverlayRect.isEmpty());
    QCOMPARE(terminalPaneRenderProbe(secondPane)
                 .unfocusedSplitOverlayRect,
             secondPane->boundingRect());

    // Zoom changes presentation but not structural split membership.
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {splitTabId, firstId, 0},
    }));
    QVERIFY(firstPane->isVisible());
    QVERIFY(!secondPane->isVisible());
    QCOMPARE(firstPane->size(), workspace->size());
    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);
    QVERIFY(!window.grabWindow().isNull());
    QCOMPARE(terminalPaneRenderProbe(firstPane)
                 .unfocusedSplitOverlayRect,
             firstPane->boundingRect());
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {splitTabId, firstId, 0},
    }));
    QVERIFY(firstPane->isVisible());
    QVERIFY(secondPane->isVisible());
    QVERIFY(!window.grabWindow().isNull());
    QCOMPARE(terminalPaneRenderProbe(firstPane)
                 .unfocusedSplitOverlayRect,
             firstPane->boundingRect());
    QCOMPARE(terminalPaneRenderProbe(secondPane)
                 .unfocusedSplitOverlayRect,
             secondPane->boundingRect());
    firstPane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), firstPane, 1000);

    // Window activity is part of actual GTK-style surface focus. The retained
    // activeFocusItem alone must not keep the old window's pane clear.
    QQuickWindow secondWindow;
    secondWindow.setColor(QColor(QStringLiteral("#281008")));
    secondWindow.resize(window.size());
    secondWindow.show();
    secondWindow.requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(secondWindow.isExposed(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(secondWindow.isActive(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isActive(), 1000);
    QVERIFY(!window.grabWindow().isNull());
    QCOMPARE(terminalPaneRenderProbe(firstPane)
                 .unfocusedSplitOverlayRect,
             firstPane->boundingRect());
    QCOMPARE(terminalPaneRenderProbe(secondPane)
                 .unfocusedSplitOverlayRect,
             secondPane->boundingRect());

    window.requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 1000);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(terminalPaneRenderProbe(firstPane)
                .unfocusedSplitOverlayRect.isEmpty());
    QCOMPARE(terminalPaneRenderProbe(secondPane)
                 .unfocusedSplitOverlayRect,
             secondPane->boundingRect());

    const quint64 firstRootBeforeSceneMove =
        terminalPaneRenderProbe(firstPane).rootSerial;
    const quint64 secondRootBeforeSceneMove =
        terminalPaneRenderProbe(secondPane).rootSerial;
    secondWindow.requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(secondWindow.isActive(), 1000);
    workspace->setParentItem(secondWindow.contentItem());
    workspace->setSize(secondWindow.size());
    firstPane->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(secondWindow.activeFocusItem(), firstPane, 1000);
    const QImage movedImage = secondWindow.grabWindow();
    QVERIFY(!movedImage.isNull());
    const TerminalPaneRenderProbeSnapshot movedFirst =
        terminalPaneRenderProbe(firstPane);
    const TerminalPaneRenderProbeSnapshot movedSecond =
        terminalPaneRenderProbe(secondPane);
    QVERIFY(movedFirst.rootSerial != firstRootBeforeSceneMove);
    QVERIFY(movedSecond.rootSerial != secondRootBeforeSceneMove);
    QVERIFY(movedFirst.unfocusedSplitOverlayRect.isEmpty());
    QCOMPARE(movedSecond.unfocusedSplitOverlayRect,
             secondPane->boundingRect());
    const QImage oldWindowImage = window.grabWindow();
    QVERIFY(!oldWindowImage.isNull());
    QVERIFY(approximatelyEqual(
        oldWindowImage.pixelColor(oldWindowImage.width() / 2,
                                  oldWindowImage.height() / 2),
        windowColor));

    // Collapsing to one leaf clears split membership immediately, including
    // when actual focus belongs to another frontend item.
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ClosePane,
        {splitTabId, secondId, 0},
    }));
    QTRY_COMPARE_WITH_TIMEOUT(workspace->findChildren<TerminalPane *>().size(),
                              2, 2000);
    // The second remaining object is the inactive tab's single pane.
    auto *secondFocusSink = new QQuickItem(secondWindow.contentItem());
    secondFocusSink->setSize(QSizeF(1.0, 1.0));
    secondFocusSink->setFocusPolicy(Qt::StrongFocus);
    secondFocusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(secondWindow.activeFocusItem(), secondFocusSink,
                              1000);
    QVERIFY(!secondWindow.grabWindow().isNull());
    QVERIFY(terminalPaneRenderProbe(firstPane)
                .unfocusedSplitOverlayRect.isEmpty());
    QVERIFY(splitDividerItems(workspace.get()).isEmpty());

    workspace.reset();
    secondWindow.close();
    window.close();
}

void TerminalWorkspaceTest::splitWorkingDirectoryPolicyReloadsForFutureNestedSplits()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/split-working-directory-XXXXXX")));
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    const QString baseDirectory = root.filePath(QStringLiteral("base"));
    const QString sourceDirectory =
        root.filePath(QStringLiteral("source directory"));
    const QString configuredFallback =
        root.filePath(QStringLiteral("configured fallback"));
    const QString reloadedFallback =
        root.filePath(QStringLiteral("reloaded fallback"));
    for (const QString &path : {
             baseDirectory, sourceDirectory, configuredFallback,
             reloadedFallback,
         }) {
        QVERIFY(QDir().mkpath(path));
    }

    const QString childLog = root.filePath(QStringLiteral("child-pwds"));
    const QString shellPath = root.filePath(QStringLiteral("record-pwd.sh"));
    QFile shell(shellPath);
    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Truncate));
    shell.write(QByteArrayLiteral("#!/bin/sh\npwd >> \"")
                + QFile::encodeName(childLog)
                + QByteArrayLiteral("\"\nexec /bin/sleep 5\n"));
    shell.close();
    QVERIFY(QFile::setPermissions(
        shellPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner));
    ShellEnvironment shellEnvironment(QFile::encodeName(shellPath));

    LaunchOptions options = baseOptions();
    options.workingDirectory = baseDirectory;
    QUrl sourceUrl;
    sourceUrl.setScheme(QStringLiteral("file"));
    sourceUrl.setHost(QStringLiteral("localhost"));
    sourceUrl.setPath(sourceDirectory);
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf '\\033]7;%s\\007' \"$1\"; sleep 5"),
        QStringLiteral("ghostty-qt-osc7"),
        sourceUrl.toString(QUrl::FullyEncoded),
    };
    options.hold = true;
    options.typography.pointSize = 12.0;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId sourceId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *sourcePane = workspace.findChild<TerminalPane *>();
    QVERIFY(sourcePane != nullptr);
    auto *sourceController =
        sourcePane->findChild<TerminalController *>();
    QVERIFY(sourceController != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(sourcePane->currentDirectory(),
                              sourceDirectory, 3000);
    sourcePane->zoomIn();
    QCOMPARE(sourcePane->fontPointSize(), 13.0);
    QSignalSpy sourceRuntime(
        sourceController, &TerminalController::runtimeOptionsRequested);

    const auto recordedDirectories = [&] {
        QFile log(childLog);
        if (!log.open(QIODevice::ReadOnly)) {
            return QStringList{};
        }
        return QString::fromUtf8(log.readAll())
            .split(u'\n', Qt::SkipEmptyParts);
    };

    LaunchOptions reloadedOptions = options;
    reloadedOptions.workingDirectory = configuredFallback;
    reloadedOptions.inheritWorkingDirectory = false;
    reloadedOptions.splitInheritWorkingDirectory = false;
    workspace.applyLaunchOptions(reloadedOptions);
    QCOMPARE(sourceRuntime.count(), 0);
    QCOMPARE(sourcePane->currentDirectory(), sourceDirectory);
    QCOMPARE(sourcePane->findChild<TerminalController *>(), sourceController);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, sourceId, 0},
    }));
    const PaneId fallbackChildId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *fallbackChild = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane != sourcePane) {
            fallbackChild = pane;
        }
    }
    QVERIFY(fallbackChild != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(recordedDirectories().size(), 1, 3000);
    QCOMPARE(recordedDirectories().constFirst(), configuredFallback);
    QCOMPARE(fallbackChild->currentDirectory(), configuredFallback);
    QCOMPARE(fallbackChild->fontPointSize(), 13.0);
    auto *fallbackController =
        fallbackChild->findChild<TerminalController *>();
    QVERIFY(fallbackController != nullptr);
    QSignalSpy fallbackRuntime(
        fallbackController, &TerminalController::runtimeOptionsRequested);

    reloadedOptions.workingDirectory = reloadedFallback;
    reloadedOptions.splitInheritWorkingDirectory = true;
    workspace.applyLaunchOptions(reloadedOptions);
    QCOMPARE(sourceRuntime.count(), 0);
    QCOMPARE(fallbackRuntime.count(), 0);
    QCOMPARE(sourcePane->currentDirectory(), sourceDirectory);
    QCOMPARE(fallbackChild->currentDirectory(), configuredFallback);

    // The explicit source matters: fallbackChild is active, but this nested
    // split still inherits the original pane's live directory and zoom.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitDown,
        {tabId, sourceId, 0},
    }));
    const PaneId inheritedChildId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(inheritedChildId != fallbackChildId);
    TerminalPane *inheritedChild = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane != sourcePane && pane != fallbackChild) {
            inheritedChild = pane;
        }
    }
    QVERIFY(inheritedChild != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(recordedDirectories().size(), 2, 3000);
    QCOMPARE(recordedDirectories().at(1), sourceDirectory);
    QCOMPARE(inheritedChild->currentDirectory(), sourceDirectory);
    QCOMPARE(inheritedChild->fontPointSize(), 13.0);

    // Clearing terminal-owned PWD makes true-mode inheritance fall back to
    // the newest global working-directory without mutating existing panes.
    QVERIFY(sourcePane->executeConfiguredAction(QStringLiteral("reset")));
    QTRY_VERIFY_WITH_TIMEOUT(sourcePane->currentDirectory().isEmpty(), 1000);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitLeft,
        {tabId, sourceId, 0},
    }));
    QTRY_COMPARE_WITH_TIMEOUT(recordedDirectories().size(), 3, 3000);
    QCOMPARE(recordedDirectories().at(2), reloadedFallback);
    QCOMPARE(sourcePane->findChild<TerminalController *>(), sourceController);
}

void TerminalWorkspaceTest::newTabInheritanceUsesStableSourceAndReloadedPolicies()
{
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir directory(QDir::current().filePath(
        QStringLiteral("tmp/tab-inheritance-XXXXXX")));
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    const QString baseDirectory = root.filePath(QStringLiteral("base"));
    const QString sourceDirectory =
        root.filePath(QStringLiteral("source directory"));
    const QString childReportedDirectory =
        root.filePath(QStringLiteral("child reported directory"));
    const QString disabledFallback =
        root.filePath(QStringLiteral("disabled fallback"));
    const QString reloadedFallback =
        root.filePath(QStringLiteral("reloaded fallback"));
    for (const QString &path : {
             baseDirectory, sourceDirectory, childReportedDirectory,
             disabledFallback, reloadedFallback,
         }) {
        QVERIFY(QDir().mkpath(path));
    }

    const QString childLog = root.filePath(QStringLiteral("child-pwds"));
    QUrl childReportedUrl;
    childReportedUrl.setScheme(QStringLiteral("file"));
    childReportedUrl.setHost(QStringLiteral("localhost"));
    childReportedUrl.setPath(childReportedDirectory);
    const QString shellPath = root.filePath(QStringLiteral("record-pwd.sh"));
    QFile shell(shellPath);
    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Truncate));
    shell.write(QByteArrayLiteral("#!/bin/sh\npwd >> \"")
                + QFile::encodeName(childLog)
                + QByteArrayLiteral("\"\nprintf '\\033]7;%s\\007' '")
                + childReportedUrl.toEncoded()
                + QByteArrayLiteral("'\nexec /bin/sleep 5\n"));
    shell.close();
    QVERIFY(QFile::setPermissions(
        shellPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner));
    ShellEnvironment shellEnvironment(QFile::encodeName(shellPath));

    QUrl sourceUrl;
    sourceUrl.setScheme(QStringLiteral("file"));
    sourceUrl.setHost(QStringLiteral("localhost"));
    sourceUrl.setPath(sourceDirectory);
    LaunchOptions options = baseOptions();
    options.workingDirectory = baseDirectory;
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf '\\033]7;%s\\007' \"$1\"; sleep 5"),
        QStringLiteral("ghostty-qt-osc7"),
        sourceUrl.toString(QUrl::FullyEncoded),
    };
    options.hold = true;
    options.typography.pointSize = 12.0;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId sourceTabId = workspace.tabModel()->idAt(0);
    const PaneId sourcePaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *sourcePane = workspace.findChild<TerminalPane *>();
    QVERIFY(sourcePane != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(sourcePane->currentDirectory(),
                              sourceDirectory, 3000);
    sourcePane->zoomIn();
    QCOMPARE(sourcePane->fontPointSize(), 13.0);

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::NewTab,
        {sourceTabId, PaneId(999'999), 0},
    }));
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::NewTab,
        {TabId(999'999), sourcePaneId, 0},
    }));
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::NewTab,
        {TabId(999'999), PaneId{}, 0},
    }));
    QCOMPARE(workspace.tabCount(), 1);

    const auto recordedDirectories = [&] {
        QFile log(childLog);
        if (!log.open(QIODevice::ReadOnly)) {
            return QStringList{};
        }
        return QString::fromUtf8(log.readAll())
            .split(u'\n', Qt::SkipEmptyParts);
    };
    const auto onlyVisiblePane = [&]() -> TerminalPane * {
        TerminalPane *result = nullptr;
        for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
            if (!pane->isVisible()) continue;
            if (result != nullptr) return nullptr;
            result = pane;
        }
        return result;
    };

    LaunchOptions reloadedOptions = options;
    reloadedOptions.workingDirectory = reloadedFallback;
    reloadedOptions.inheritWorkingDirectory = false;
    reloadedOptions.tabInheritWorkingDirectory = true;
    reloadedOptions.windowInheritFontSize = true;
    reloadedOptions.typography.pointSize = 10.0;
    workspace.applyLaunchOptions(reloadedOptions);
    QCOMPARE(sourcePane->fontPointSize(), 13.0);
    QCOMPARE(sourcePane->currentDirectory(), sourceDirectory);

    // Create a second source in the original tab. Its terminal report and
    // manual zoom differ from the first pane so broad fanout can prove that
    // every new tab retains its explicit action source.
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {sourceTabId, sourcePaneId, 0},
    }));
    const PaneId splitPaneId =
        workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(splitPaneId != sourcePaneId);
    TerminalPane *splitPane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane != sourcePane) splitPane = pane;
    }
    QVERIFY(splitPane != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(recordedDirectories().size(), 1, 3000);
    QCOMPARE(recordedDirectories().constFirst(), sourceDirectory);
    QTRY_COMPARE_WITH_TIMEOUT(splitPane->currentDirectory(),
                              childReportedDirectory, 3000);
    QCOMPARE(splitPane->fontPointSize(), 13.0);
    splitPane->zoomIn();
    QCOMPARE(splitPane->fontPointSize(), 14.0);

    QFile clearLog(childLog);
    QVERIFY(clearLog.open(QIODevice::WriteOnly | QIODevice::Truncate));
    clearLog.close();

    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("new_tab")));
    QCOMPARE(workspace.tabCount(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(recordedDirectories().size(), 2, 3000);
    QStringList broadDirectories = recordedDirectories();
    QStringList expectedBroadDirectories{
        sourceDirectory, childReportedDirectory,
    };
    broadDirectories.sort();
    expectedBroadDirectories.sort();
    QCOMPARE(broadDirectories, expectedBroadDirectories);

    workspace.setCurrentIndex(1);
    TerminalPane *firstBroadChild = onlyVisiblePane();
    QVERIFY(firstBroadChild != nullptr);
    QCOMPARE(firstBroadChild->fontPointSize(), 13.0);
    workspace.setCurrentIndex(2);
    TerminalPane *secondBroadChild = onlyVisiblePane();
    QVERIFY(secondBroadChild != nullptr);
    QCOMPARE(secondBroadChild->fontPointSize(), 14.0);

    // Reloaded false policies affect only subsequent inheritance decisions.
    // Existing working-directory reports remain intact; unadjusted children
    // still follow the ordinary live font-size setting.
    reloadedOptions.workingDirectory = disabledFallback;
    reloadedOptions.tabInheritWorkingDirectory = false;
    reloadedOptions.windowInheritFontSize = false;
    reloadedOptions.typography.pointSize = 9.0;
    workspace.applyLaunchOptions(reloadedOptions);
    QCOMPARE(sourcePane->currentDirectory(), sourceDirectory);
    QCOMPARE(splitPane->currentDirectory(), childReportedDirectory);
    QCOMPARE(sourcePane->fontPointSize(), 13.0);
    QCOMPARE(splitPane->fontPointSize(), 14.0);
    QCOMPARE(firstBroadChild->fontPointSize(), 9.0);
    QCOMPARE(secondBroadChild->fontPointSize(), 9.0);

    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 4);
    QTRY_COMPARE_WITH_TIMEOUT(recordedDirectories().size(), 3, 3000);
    QCOMPARE(recordedDirectories().at(2), disabledFallback);
    TerminalPane *disabledChild = onlyVisiblePane();
    QVERIFY(disabledChild != nullptr);
    QCOMPARE(disabledChild->fontPointSize(), 9.0);

    // Re-enable both policies, then use the original split tab. Empty-context
    // creation mirrors the QML button and must select its recorded active leaf.
    reloadedOptions.workingDirectory = reloadedFallback;
    reloadedOptions.tabInheritWorkingDirectory = true;
    reloadedOptions.windowInheritFontSize = true;
    reloadedOptions.typography.pointSize = 8.0;
    workspace.applyLaunchOptions(reloadedOptions);
    QCOMPARE(disabledChild->fontPointSize(), 8.0);
    workspace.setCurrentIndex(0);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, splitPaneId);
    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 5);
    QTRY_COMPARE_WITH_TIMEOUT(recordedDirectories().size(), 4, 3000);
    QCOMPARE(recordedDirectories().at(3), childReportedDirectory);
    TerminalPane *activeLeafChild = onlyVisiblePane();
    QVERIFY(activeLeafChild != nullptr);
    QCOMPARE(activeLeafChild->fontPointSize(), 14.0);

    // An explicitly cleared OSC 7 report falls back to the newest configured
    // directory while the independent font-size policy still inherits.
    QVERIFY(activeLeafChild->executeConfiguredAction(QStringLiteral("reset")));
    QTRY_VERIFY_WITH_TIMEOUT(activeLeafChild->currentDirectory().isEmpty(),
                             1000);
    workspace.newTab();
    QCOMPARE(workspace.tabCount(), 6);
    QTRY_COMPARE_WITH_TIMEOUT(recordedDirectories().size(), 5, 3000);
    QCOMPARE(recordedDirectories().at(4), reloadedFallback);
    TerminalPane *clearedFallbackChild = onlyVisiblePane();
    QVERIFY(clearedFallbackChild != nullptr);
    QCOMPARE(clearedFallbackChild->fontPointSize(), 14.0);

    // The built-in binding emits a signal rather than a typed action. Send it
    // to the non-active source pane and prove its identity is not discarded.
    workspace.setCurrentIndex(0);
    QKeyEvent newTabPress(
        QEvent::KeyPress, Qt::Key_T,
        Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("T"));
    QCoreApplication::sendEvent(sourcePane, &newTabPress);
    QKeyEvent newTabRelease(
        QEvent::KeyRelease, Qt::Key_T,
        Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("T"));
    QCoreApplication::sendEvent(sourcePane, &newTabRelease);
    QCOMPARE(workspace.tabCount(), 7);
    QTRY_COMPARE_WITH_TIMEOUT(recordedDirectories().size(), 6, 3000);
    QCOMPARE(recordedDirectories().at(5), sourceDirectory);
    TerminalPane *bindingChild = onlyVisiblePane();
    QVERIFY(bindingChild != nullptr);
    QCOMPARE(bindingChild->fontPointSize(), 13.0);
}

void TerminalWorkspaceTest::splitDividerHitRegionPreservesTerminalInputAndZoom()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(902, 602);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);

    const TabId tabId = workspace->tabModel()->idAt(0);
    const PaneId firstId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace->findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    const PaneId secondId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != firstPane) secondPane = pane;
    }
    QVERIFY(secondPane != nullptr);

    TerminalController *firstController =
        firstPane->findChild<TerminalController *>();
    TerminalController *secondController =
        secondPane->findChild<TerminalController *>();
    QVERIFY(firstController != nullptr);
    QVERIFY(secondController != nullptr);
    QSignalSpy firstSelectionBegun(
        firstController, &TerminalController::beginSelectionRequested);
    QSignalSpy firstSelectionEnded(
        firstController, &TerminalController::endSelectionRequested);
    QSignalSpy secondSelectionBegun(
        secondController, &TerminalController::beginSelectionRequested);
    QSignalSpy secondSelectionEnded(
        secondController, &TerminalController::endSelectionRequested);

    const qreal dividerEdge = firstPane->x() + firstPane->width();
    QCOMPARE(secondPane->x(), dividerEdge + 2.0);
    const QList<QQuickItem *> dividers = splitDividerItems(workspace.get());
    QCOMPARE(dividers.size(), 1);
    QQuickItem *divider = dividers.constFirst();
    QCOMPARE(divider->position(), QPointF(dividerEdge, 0.0));
    QCOMPARE(divider->size(), QSizeF(2.0, workspace->height()));
    QCOMPARE(divider->z(), 1.0);

    const qreal pointerY = workspace->height() / 3.0;
    const QPoint gapPoint = windowPoint(
        workspace.get(), QPointF(dividerEdge + 1.0, pointerY));
    QTest::mouseMove(&window, gapPoint);
    QTRY_COMPARE_WITH_TIMEOUT(window.cursor().shape(), Qt::SplitHCursor, 1000);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, gapPoint);
    QVERIFY(window.mouseGrabberItem() == divider);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, gapPoint);
    QCOMPARE(firstSelectionBegun.count(), 0);
    QCOMPARE(firstSelectionEnded.count(), 0);
    QCOMPARE(secondSelectionBegun.count(), 0);
    QCOMPARE(secondSelectionEnded.count(), 0);

    const QPoint firstCellPoint = windowPoint(
        workspace.get(), QPointF(dividerEdge - 1.0, pointerY));
    QTest::mouseMove(&window, firstCellPoint);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier,
                      firstCellPoint);
    QVERIFY(window.mouseGrabberItem() == firstPane);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                        firstCellPoint);
    QCOMPARE(firstSelectionBegun.count(), 1);
    QCOMPARE(firstSelectionEnded.count(), 1);

    const QPoint secondCellPoint = windowPoint(
        workspace.get(), QPointF(secondPane->x() + 1.0, pointerY));
    QTest::mouseMove(&window, secondCellPoint);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier,
                      secondCellPoint);
    QVERIFY(window.mouseGrabberItem() == secondPane);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                        secondCellPoint);
    QCOMPARE(secondSelectionBegun.count(), 1);
    QCOMPARE(secondSelectionBegun.constFirst().constFirst().toInt(), 0);
    QCOMPARE(secondSelectionEnded.count(), 1);
    QCOMPARE(workspace->tabModel()->entryAt(0)->activePaneId, secondId);

    const QSizeF normalFirstSize = firstPane->size();
    const QSizeF normalSecondSize = secondPane->size();
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QVERIFY(splitDividerItems(workspace.get()).isEmpty());
    QCOMPARE(secondPane->size(), workspace->size());

    QTest::mouseMove(&window, gapPoint);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, gapPoint);
    QVERIFY(window.mouseGrabberItem() == secondPane);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, gapPoint);
    QCOMPARE(secondSelectionBegun.count(), 2);
    QCOMPARE(secondSelectionEnded.count(), 2);

    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QCOMPARE(firstPane->size(), normalFirstSize);
    QCOMPARE(secondPane->size(), normalSecondSize);
    QCOMPARE(splitDividerItems(workspace.get()).size(), 1);

    workspace->newTab();
    QCOMPARE(workspace->currentIndex(), 1);
    QVERIFY(splitDividerItems(workspace.get()).isEmpty());
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ActivateTab,
        {tabId, PaneId{}, 0},
    }));
    QCOMPARE(splitDividerItems(workspace.get()).size(), 1);

    // Moving a grabbed workspace to another scene destroys the old handle,
    // releases the old window's delivery-agent grab, and recreates a fresh
    // handle from the stable split ID in the new scene.
    QQuickWindow secondWindow;
    secondWindow.resize(window.size());
    secondWindow.show();
    QTRY_VERIFY_WITH_TIMEOUT(secondWindow.isExposed(), 1000);
    const QSizeF firstBeforeReparent = firstPane->size();
    const QSizeF secondBeforeReparent = secondPane->size();
    const qreal reparentEdge = firstPane->x() + firstPane->width();
    const QPoint oldSceneStart = windowPoint(
        workspace.get(), QPointF(reparentEdge + 1.0, pointerY));
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, oldSceneStart);
    QVERIFY(window.mouseGrabberItem() != nullptr);
    workspace->setParentItem(secondWindow.contentItem());
    workspace->setSize(secondWindow.size());
    QVERIFY(window.mouseGrabberItem() == nullptr);
    QVERIFY(secondWindow.mouseGrabberItem() == nullptr);
    QCOMPARE(firstPane->size(), firstBeforeReparent);
    QCOMPARE(secondPane->size(), secondBeforeReparent);
    const QPoint staleMove = oldSceneStart + QPoint(60, 0);
    QTest::mouseMove(&window, staleMove);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, staleMove);
    QCOMPARE(firstPane->size(), firstBeforeReparent);
    QCOMPARE(secondPane->size(), secondBeforeReparent);

    QCOMPARE(splitDividerItems(workspace.get()).size(), 1);
    const QPoint newSceneStart = windowPoint(
        workspace.get(), QPointF(reparentEdge + 1.0, pointerY));
    const QPoint newSceneEnd = newSceneStart + QPoint(60, 0);
    QTest::mousePress(&secondWindow, Qt::LeftButton, Qt::NoModifier,
                      newSceneStart);
    QVERIFY(secondWindow.mouseGrabberItem() != nullptr);
    QTest::mouseMove(&secondWindow, newSceneEnd);
    QTest::mouseRelease(&secondWindow, Qt::LeftButton, Qt::NoModifier,
                        newSceneEnd);
    QVERIFY(qAbs(firstPane->width()
                 - (firstBeforeReparent.width() + 60.0)) <= 1.0);
    QVERIFY(qAbs(secondPane->width()
                 - (secondBeforeReparent.width() - 60.0)) <= 1.0);

    workspace.reset();
    secondWindow.close();
    window.close();
}

void TerminalWorkspaceTest::splitDividerDragClampsPersistsAndCancels()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    QQuickWindow window;
    window.resize(602, 903);
    auto workspace = std::make_unique<TerminalWorkspace>();
    workspace->setParentItem(window.contentItem());
    workspace->setSize(window.size());
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->tabCount(), 1, 1000);

    const TabId tabId = workspace->tabModel()->idAt(0);
    const PaneId firstId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *topPane = workspace->findChild<TerminalPane *>();
    QVERIFY(topPane != nullptr);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::SplitDown,
        {tabId, firstId, 0},
    }));
    const PaneId bottomId = workspace->tabModel()->entryAt(0)->activePaneId;
    TerminalPane *bottomPane = nullptr;
    for (TerminalPane *pane : workspace->findChildren<TerminalPane *>()) {
        if (pane != topPane) bottomPane = pane;
    }
    QVERIFY(bottomPane != nullptr);
    QCOMPARE(splitDividerItems(workspace.get()).size(), 1);

    const auto dividerPoint = [&] {
        return windowPoint(
            workspace.get(),
            QPointF(workspace->width() / 2.0,
                    topPane->y() + topPane->height() + 1.0));
    };

    // A press/release without pointer movement must preserve the fractional
    // ratio instead of quantizing it to the divider's floored pixel.
    const QSizeF initialTopSize = topPane->size();
    const QSizeF initialBottomSize = bottomPane->size();
    QPoint start = dividerPoint();
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, start);
    QCOMPARE(topPane->size(), initialTopSize);
    QCOMPARE(bottomPane->size(), initialBottomSize);

    window.resize(602, 1003);
    workspace->setSize(window.size());
    QCOMPARE(topPane->height(), 500.0);
    QCOMPARE(bottomPane->height(), 501.0);

    window.resize(602, 903);
    workspace->setSize(window.size());
    QCOMPARE(topPane->height(), 450.0);
    QCOMPARE(bottomPane->height(), 451.0);

    const qreal topHeightBeforeDrag = topPane->height();
    start = dividerPoint();
    const QPoint moved = start + QPoint(0, 60);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&window, moved);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, moved);
    QCOMPARE(topPane->height(), topHeightBeforeDrag + 60.0);

    window.resize(602, 1003);
    workspace->setSize(window.size());
    QCOMPARE(topPane->height(), 567.0);
    QCOMPARE(bottomPane->y(), topPane->height() + 2.0);

    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::EqualizeSplits,
        {tabId, bottomId, 0},
    }));
    QVERIFY(qAbs(topPane->height() - bottomPane->height()) <= 1.0);
    const QSizeF equalTopSize = topPane->size();
    const QSizeF equalBottomSize = bottomPane->size();

    // An explicit grab cancellation leaves the last accepted ratio intact
    // and prevents later pointer movement from mutating it.
    start = dividerPoint();
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QQuickItem *grabber = window.mouseGrabberItem();
    QVERIFY(grabber != nullptr);
    QCOMPARE(grabber->objectName(), QStringLiteral("_ghosttyQtSplitDivider"));
    grabber->ungrabMouse();
    QVERIFY(window.mouseGrabberItem() == nullptr);
    const QPoint canceledMove = start + QPoint(0, 120);
    QTest::mouseMove(&window, canceledMove);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                        canceledMove);
    QCOMPARE(topPane->size(), equalTopSize);
    QCOMPARE(bottomPane->size(), equalBottomSize);

    // Zooming during a drag destroys the visible handle and must relinquish
    // its grab without changing focus or the stored split ratio.
    start = dividerPoint();
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QVERIFY(window.mouseGrabberItem() != nullptr);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, bottomId, 0},
    }));
    QVERIFY(splitDividerItems(workspace.get()).isEmpty());
    QVERIFY(window.mouseGrabberItem() == nullptr);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, start);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, bottomId, 0},
    }));
    QCOMPARE(topPane->size(), equalTopSize);
    QCOMPARE(bottomPane->size(), equalBottomSize);

    // Both endpoints are legal and the edge handle remains available to
    // recover a collapsed child.
    start = dividerPoint();
    const QPoint topEdge = windowPoint(
        workspace.get(), QPointF(workspace->width() / 2.0, 0.0));
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&window, topEdge);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, topEdge);
    QCOMPARE(topPane->height(), 0.0);
    QCOMPARE(bottomPane->y(), 2.0);
    QCOMPARE(splitDividerItems(workspace.get()).size(), 1);

    start = dividerPoint();
    const QPoint bottomEdge = windowPoint(
        workspace.get(),
        QPointF(workspace->width() / 2.0, workspace->height() - 1.0));
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&window, bottomEdge);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, bottomEdge);
    QCOMPARE(topPane->height(), workspace->height() - 2.0);
    QCOMPARE(bottomPane->height(), 0.0);

    start = dividerPoint();
    const QPoint center = windowPoint(
        workspace.get(),
        QPointF(workspace->width() / 2.0, workspace->height() / 2.0));
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&window, center);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, center);
    QVERIFY(qAbs(topPane->height() - bottomPane->height()) <= 1.0);

    workspace->setHeight(2.0);
    QVERIFY(splitDividerItems(workspace.get()).isEmpty());
    const QPoint zeroExtent = windowPoint(
        workspace.get(), QPointF(workspace->width() / 2.0, 1.0));
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, zeroExtent);
    QVERIFY(window.mouseGrabberItem() == nullptr);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, zeroExtent);

    // Tree collapse invalidates the target node, but its stable ID lets the
    // workspace remove the grabbed handle without retaining a dangling node.
    workspace->setHeight(1002.0);
    QCOMPARE(splitDividerItems(workspace.get()).size(), 1);
    start = dividerPoint();
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QVERIFY(window.mouseGrabberItem() != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(!topPane->isRunning(), 1000);
    QPointer<TerminalPane> closedPane(topPane);
    QPointer<TerminalPane> survivingPane(bottomPane);
    QVERIFY(workspace->dispatchAction({
        WorkspaceAction::ClosePane,
        {tabId, firstId, 0},
    }));
    QVERIFY(window.mouseGrabberItem() == nullptr);
    QVERIFY(splitDividerItems(workspace.get()).isEmpty());
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTRY_VERIFY_WITH_TIMEOUT(closedPane.isNull(), 1000);
    QVERIFY(!survivingPane.isNull());
    QCOMPARE(workspace->findChildren<TerminalPane *>(),
             QList<TerminalPane *>{survivingPane});
    QCOMPARE(survivingPane->position(), QPointF());
    QCOMPARE(survivingPane->size(), workspace->size());

    workspace.reset();
    window.close();
}

void TerminalWorkspaceTest::splitZoomPreservesLayoutAndResetsOnNavigationAndSplit()
{
    // Keep later split/tab shells alive while the explicitly held /bin/true
    // source becomes a stopped pane that can be closed without confirmation.
    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId firstId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);

    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, firstId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    const PaneId secondId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane) secondPane = candidate;
    }
    QVERIFY(secondPane != nullptr);
    const QSizeF normalFirstSize = firstPane->size();
    const QSizeF normalSecondSize = secondPane->size();

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QVERIFY(!firstPane->isVisible());
    QVERIFY(secondPane->isVisible());
    QCOMPARE(secondPane->position(), workspace.boundingRect().topLeft());
    QCOMPARE(secondPane->size(), workspace.boundingRect().size());

    workspace.newTab();
    QCOMPARE(workspace.currentIndex(), 1);
    QVERIFY(!firstPane->isVisible());
    QVERIFY(!secondPane->isVisible());
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateTab,
        {tabId, PaneId{}, 0},
    }));
    QCOMPARE(workspace.currentIndex(), 0);
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QVERIFY(!firstPane->isVisible());
    QVERIFY(secondPane->isVisible());
    QCOMPARE(secondPane->size(), workspace.boundingRect().size());

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {tabId, firstId, 0},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, firstId);
    QVERIFY(firstPane->isVisible());
    QVERIFY(secondPane->isVisible());

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {tabId, secondId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QVERIFY(firstPane->isVisible());
    QVERIFY(secondPane->isVisible());
    QCOMPARE(firstPane->size(), normalFirstSize);
    QCOMPARE(secondPane->size(), normalSecondSize);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, secondId, 1},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, firstId);
    QVERIFY(firstPane->isVisible());
    QVERIFY(secondPane->isVisible());

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, firstId, 0},
    }));
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 4);
    int visiblePaneCount = 0;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane->isVisible()) ++visiblePaneCount;
    }
    // The second tab remains hidden; every pane in the current tab is visible
    // after splitting resets zoom.
    QCOMPARE(visiblePaneCount, 3);

    const PaneId zoomTarget = workspace.tabModel()->entryAt(0)->activePaneId;
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, zoomTarget, 0},
    }));
    TerminalPane *zoomedPane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane->isVisible()) zoomedPane = pane;
    }
    QVERIFY(zoomedPane != nullptr);
    QPointer<TerminalPane> zoomedPaneGuard(zoomedPane);
    QTRY_VERIFY_WITH_TIMEOUT(!firstPane->isRunning(), 1000);
    const int paneCountBeforeClose =
        workspace.findChildren<TerminalPane *>().size();
    QPointer<TerminalPane> closedPane(firstPane);
    QSignalSpy confirmation(&workspace,
                            &TerminalWorkspace::closeConfirmationRequested);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ClosePane,
        {tabId, firstId, 0},
    }));
    QCOMPARE(confirmation.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.findChildren<TerminalPane *>().size(),
                              paneCountBeforeClose - 1,
                              1000);
    QTRY_VERIFY_WITH_TIMEOUT(closedPane.isNull(), 1000);
    QCOMPARE(workspace.tabCount(), 2);
    QCOMPARE(workspace.tabModel()->count(), 2);
    const TabListEntry *survivingTab = workspace.tabModel()->entryAt(0);
    QVERIFY(survivingTab != nullptr);
    QVERIFY(survivingTab->zoomed);
    QCOMPARE(survivingTab->activePaneId, zoomTarget);
    QVERIFY(!zoomedPaneGuard.isNull());
    QVERIFY(zoomedPaneGuard->isVisible());
}

void TerminalWorkspaceTest::splitZoomNavigationPolicyReloadsLive()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/sh"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.splitPreserveZoomNavigation = false;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId firstId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    const PaneId secondId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *pane : workspace.findChildren<TerminalPane *>()) {
        if (pane != firstPane) secondPane = pane;
    }
    QVERIFY(secondPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {tabId, firstId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, firstId, 0},
    }));
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, firstId);

    LaunchOptions reloadedOptions = options;
    reloadedOptions.splitPreserveZoomNavigation = true;
    workspace.applyLaunchOptions(reloadedOptions);

    // Reload changes policy only. The currently zoomed pane and logical split
    // tree remain untouched until a successful navigation occurs.
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, firstId);
    QVERIFY(firstPane->isVisible());
    QVERIFY(!secondPane->isVisible());

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, firstId, 1},
    }));
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, secondId);
    QVERIFY(!firstPane->isVisible());
    QVERIFY(secondPane->isVisible());
    QCOMPARE(secondPane->position(), workspace.boundingRect().topLeft());
    QCOMPARE(secondPane->size(), workspace.boundingRect().size());

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePane,
        {tabId, secondId, Qt::Key_Left},
    }));
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, firstId);
    QVERIFY(firstPane->isVisible());
    QVERIFY(!secondPane->isVisible());
    QCOMPARE(firstPane->position(), workspace.boundingRect().topLeft());
    QCOMPARE(firstPane->size(), workspace.boundingRect().size());

    // No-op and invalid navigation are inert, including while zoomed.
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, firstId, 2},
    }));
    QVERIFY(!workspace.dispatchAction({
        WorkspaceAction::NavigatePane,
        {tabId, firstId, Qt::Key_Home},
    }));
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, firstId);
    QVERIFY(firstPane->isVisible());
    QVERIFY(!secondPane->isVisible());

    reloadedOptions.splitPreserveZoomNavigation = false;
    workspace.applyLaunchOptions(reloadedOptions);
    QVERIFY(workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, firstId);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::NavigatePaneRelative,
        {tabId, firstId, 1},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, secondId);
    QVERIFY(firstPane->isVisible());
    QVERIFY(secondPane->isVisible());

    reloadedOptions.splitPreserveZoomNavigation = true;
    workspace.applyLaunchOptions(reloadedOptions);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, secondId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivatePane,
        {tabId, firstId, 0},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, firstId);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ToggleSplitZoom,
        {tabId, firstId, 0},
    }));
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitDown,
        {tabId, firstId, 0},
    }));
    QVERIFY(!workspace.tabModel()->entryAt(0)->zoomed);
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 3);
}

void TerminalWorkspaceTest::broadContainerActionsResolveFromActivePane()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    // This aspect makes the first column wide while the two nested right-hand
    // columns are tall, which also lets the broad-auto assertion below
    // distinguish source resolution from active-pane placement.
    workspace.setSize(QSizeF(1802.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId tabId = workspace.tabModel()->idAt(0);
    const PaneId firstId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, firstId, 0},
    }));
    const PaneId secondId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane) secondPane = candidate;
    }
    QVERIFY(secondPane != nullptr);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {tabId, secondId, 0},
    }));
    const PaneId thirdId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *thirdPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane && candidate != secondPane) {
            thirdPane = candidate;
        }
    }
    QVERIFY(thirdPane != nullptr);

    const qreal firstWidth = firstPane->width();
    const qreal secondWidth = secondPane->width();
    const qreal thirdWidth = thirdPane->width();
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("resize_split:right,10")));

    // All three snapshot surfaces resolve resize from the tree's active third
    // pane, so only its nearest divider moves, three times.
    QVERIFY(qAbs(firstPane->width() - firstWidth) <= 1.0);
    QVERIFY(qAbs(secondPane->width() - (secondWidth + 30.0)) <= 1.0);
    QVERIFY(qAbs(thirdPane->width() - (thirdWidth - 30.0)) <= 1.0);
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, thirdId);

    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("goto_split:previous")));
    // Three wrapped moves across three panes return to the original active
    // pane; resolving from each snapshot source would end on a different ID.
    QCOMPARE(workspace.tabModel()->entryAt(0)->activePaneId, thirdId);

    const qreal thirdWidthBeforeAuto = thirdPane->width();
    const qreal thirdHeightBeforeAuto = thirdPane->height();
    QVERIFY(workspace.executeSurfaceActionOnAllPanes(
        QStringLiteral("new_split")));
    QCOMPARE(workspace.findChildren<TerminalPane *>().size(), 6);
    // The first snapshot source is wider than tall, so its auto direction is
    // right even though placement is redirected to the active third pane.
    // Later tall sources split the evolving active branch downward. If auto
    // were resolved after redirection, the third pane would be cut vertically
    // by the first action instead of retaining its full height.
    QVERIFY(thirdPane->width() < thirdWidthBeforeAuto);
    QVERIFY(qAbs(thirdPane->height() - thirdHeightBeforeAuto) <= 1.0);
}

void TerminalWorkspaceTest::inactiveTabResizeAppliesWhenActivated()
{
    ShellEnvironment shell(QByteArrayLiteral("/bin/true"));
    LaunchOptions options = baseOptions();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    TerminalWorkspace::setDefaultLaunchOptions(options);

    TerminalWorkspace workspace;
    workspace.setSize(QSizeF(902.0, 602.0));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.tabCount(), 1, 1000);
    const TabId firstTab = workspace.tabModel()->idAt(0);
    const PaneId firstId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *firstPane = workspace.findChild<TerminalPane *>();
    QVERIFY(firstPane != nullptr);
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::SplitRight,
        {firstTab, firstId, 0},
    }));
    const PaneId secondId = workspace.tabModel()->entryAt(0)->activePaneId;
    TerminalPane *secondPane = nullptr;
    for (TerminalPane *candidate : workspace.findChildren<TerminalPane *>()) {
        if (candidate != firstPane) secondPane = candidate;
    }
    QVERIFY(secondPane != nullptr);
    const qreal firstWidth = firstPane->width();
    const qreal secondWidth = secondPane->width();

    workspace.newTab();
    QCOMPARE(workspace.currentIndex(), 1);
    QVERIFY(!firstPane->isVisible());
    QVERIFY(!secondPane->isVisible());
    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ResizeSplit,
        {firstTab, secondId, Qt::Key_Right, 90},
    }));
    // Hidden panes retain their last applied PTY size until their tab is shown.
    QCOMPARE(firstPane->width(), firstWidth);
    QCOMPARE(secondPane->width(), secondWidth);

    QVERIFY(workspace.dispatchAction({
        WorkspaceAction::ActivateTab,
        {firstTab, PaneId{}, 0},
    }));
    QVERIFY(qAbs(firstPane->width() - (firstWidth + 90.0)) <= 1.0);
    QVERIFY(qAbs(secondPane->width() - (secondWidth - 90.0)) <= 1.0);
}

QTEST_MAIN(TerminalWorkspaceTest)

#include "test_terminal_workspace.moc"
