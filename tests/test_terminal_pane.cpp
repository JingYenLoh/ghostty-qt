#include "launch_options.h"
#include "terminal_cell_metrics.h"
#include "terminal_clipboard.h"
#include "terminal_controller.h"
#include "terminal_custom_shader_pipeline.h"
#include "terminal_drop.h"
#include "terminal_geometry.h"
#include "terminal_inspector_model.h"
#include "terminal_pane.h"
#include "terminal_pane_render_probe_p.h"
#include "terminal_types.h"
#include "terminal_workspace.h"

#include <QChronoTimer>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMetaMethod>
#include <QMimeData>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <qqml.h>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
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

qreal rectangleArea(const QRectF &rect)
{
    return rect.width() * rect.height();
}

qreal totalRectangleArea(const QVector<QRectF> &rects)
{
    qreal total = 0.0;
    for (const QRectF &rect : rects) {
        total += rectangleArea(rect);
    }
    return total;
}

bool rectanglesArePairwiseDisjoint(const QVector<QRectF> &rects)
{
    for (qsizetype left = 0; left < rects.size(); ++left) {
        for (qsizetype right = left + 1; right < rects.size(); ++right) {
            if (rectangleArea(
                    rects.at(left).intersected(rects.at(right))) > 0.0) {
                return false;
            }
        }
    }
    return true;
}

bool rectanglesFitInside(const QVector<QRectF> &rects, const QRectF &bounds)
{
    return std::ranges::all_of(rects, [&bounds](const QRectF &rect) {
        return !rect.isEmpty() && bounds.contains(rect);
    });
}

QRectF expectedSpriteCanvas(const QRectF &sprite, qreal devicePixelRatio)
{
    const qreal dpr = std::isfinite(devicePixelRatio)
            && devicePixelRatio > 0.0
        ? devicePixelRatio : 1.0;
    const qint64 physicalWidth = qRound64(sprite.width() * dpr);
    const qint64 physicalHeight = qRound64(sprite.height() * dpr);
    const qreal horizontalPadding =
        static_cast<qreal>(physicalWidth / 4) / dpr;
    const qreal verticalPadding =
        static_cast<qreal>(physicalHeight / 4) / dpr;
    return sprite.adjusted(
        -horizontalPadding, -verticalPadding,
        horizontalPadding, verticalPadding);
}

void useSystemFixedFont(LaunchOptions &options)
{
    options.typography.face(TerminalFontRole::Regular).families = {
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family(),
    };
}

TerminalUpdate fullFrameWithScrollbar(quint64 total, quint64 offset,
                                      quint64 length, quint64 revision = 1)
{
    TerminalUpdate update;
    update.columns = 2;
    update.rows = 2;
    update.fullFrame = true;
    update.scrollbarChanged = true;
    update.scrollTotal = total;
    update.scrollOffset = offset;
    update.scrollLength = length;
    update.contentRevision = revision;
    for (int row = 0; row < update.rows; ++row) {
        TerminalRowUpdate rowUpdate;
        rowUpdate.row = row;
        rowUpdate.cells.resize(update.columns);
        update.dirtyRows.append(std::move(rowUpdate));
    }
    return update;
}

TerminalUpdate scrollbarMetadata(quint64 total, quint64 offset, quint64 length,
                                 quint64 revision)
{
    TerminalUpdate update;
    update.columns = 2;
    update.rows = 2;
    update.scrollbarChanged = true;
    update.scrollTotal = total;
    update.scrollOffset = offset;
    update.scrollLength = length;
    update.contentRevision = revision;
    return update;
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

bool sendWheelEvent(TerminalPane &pane, const QPoint &pixelDelta,
                    const QPoint &angleDelta,
                    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF position(8.0, 8.0);
    QWheelEvent event(position, position, pixelDelta, angleDelta, Qt::NoButton,
                      modifiers, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&pane, &event);
    return event.isAccepted();
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

GhosttyKeybindTrigger generationTestKey(
    quint32 codepoint, quint8 modifiers = 0)
{
    return {
        .kind = GhosttyKeybindKeyKind::Unicode,
        .unicodeCodepoint = codepoint,
        .modifiers = modifiers,
    };
}

GhosttyKeybindDefinition generationTestBinding(
    QVector<GhosttyKeybindTrigger> sequence, QString action)
{
    return {
        .sequence = std::move(sequence),
        .actions = {std::move(action)},
    };
}

GhosttyKeybindConfig generationTestConfig()
{
    GhosttyKeybindConfig config;
    config.root = {
        generationTestBinding(
            {generationTestKey('o', GhosttyKeybindCtrl)},
            QStringLiteral("activate_key_table_once:once")),
    };
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("edit"),
            .bindings = {
                generationTestBinding(
                    {generationTestKey('x'), generationTestKey('y')},
                    QStringLiteral("new_tab")),
            },
        },
        GhosttyKeybindTable{
            .name = QStringLiteral("once"),
            .bindings = {
                generationTestBinding(
                    {generationTestKey('x'), generationTestKey('y')},
                    QStringLiteral("new_tab")),
            },
        },
    };
    return config;
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

std::shared_ptr<const TerminalKittyGraphicsImage>
kittyImage(quint64 generation, const QColor &color, int opacity = 255)
{
    QImage rgba(QSize(2, 2), QImage::Format_RGBA8888);
    QColor packed = color;
    packed.setAlpha(opacity);
    rgba.fill(packed);
    const bool fullyOpaque = opacity == 255;
    return std::make_shared<const TerminalKittyGraphicsImage>(
        TerminalKittyGraphicsImage{
            .imageId = 1,
            .generation = generation,
            .fullyOpaque = fullyOpaque,
            .straightRgba = std::move(rgba),
        });
}

std::shared_ptr<const TerminalKittyGraphicsImage>
verticalKittyImage(quint64 generation)
{
    QImage rgba(QSize(2, 2), QImage::Format_RGBA8888);
    for (int column = 0; column < 2; ++column) {
        rgba.setPixelColor(column, 0, QColor(255, 0, 0, 128));
        rgba.setPixelColor(column, 1, QColor(0, 0, 255, 255));
    }
    return std::make_shared<const TerminalKittyGraphicsImage>(
        TerminalKittyGraphicsImage{
            .imageId = 1,
            .generation = generation,
            .straightRgba = std::move(rgba),
        });
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

struct PaneBellDeviceState {
    int systemBells = 0;
    QList<GhosttyConfigPath> sources;
    QList<double> volumes;
    int restarts = 0;
};

class FakePaneBellDevice final : public TerminalBellDevice {
public:
    explicit FakePaneBellDevice(PaneBellDeviceState &state)
        : state_(state)
    {}

    void ringSystemBell() override { ++state_.systemBells; }

    bool setAudioSource(const GhosttyConfigPath &source) override
    {
        state_.sources.append(source);
        return true;
    }

    void setAudioVolume(double volume) override
    {
        state_.volumes.append(volume);
    }

    bool restartAudio() override
    {
        ++state_.restarts;
        return true;
    }

private:
    PaneBellDeviceState &state_;
};

} // namespace

class TerminalPaneTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void disabledCustomShadersAvoidOffscreenRenderLayers();
    void delegatedCustomShaderRenderingTearsDownDirectPaintNode();
    void presentsScrollbarFromRetainedMetadata();
    void presentsProgressReportsAndAppliesLivePolicy();
    void routesWaitAfterCommandDismissalSeparatelyFromHold();
    void presentsAbnormalExitUntilExplicitDismissal();
    void presentsChildExecFailureEndToEnd();
    void movesScrollbarWithClampedAbsoluteRows();
    void reloadsScrollbarPolicyAndHandlesHugeCounts();
    void latchesClearsAndReloadsBellFeedback();
    void routesAudibleBellEffectsForEveryEventAndReload();
    void audibleBellLeaseSurvivesDestructiveObserver();
    void resizeOverlayPositions_data();
    void resizeOverlayPositions();
    void resizeOverlayCoalescesAndRestarts();
    void retainsStartingTextAcrossNoFramePaints();
    void replacesStartingFrameInsteadOfAccumulatingSceneRoots();
    void reloadsFontWithoutOverwritingManualZoom();
    void reloadsTypographyNonCumulatively();
    void refreshesFontProgramAfterDatabaseChange();
    void executesTypedFontSizeActions();
    void workspaceActionHandlerRetainsMutableState();
    void packagesInputMethodLifecycleAsOneWorkerRequest();
    void writesClipboardDestinations();
    void configuredTitleReapplicationRestoresBaseLayer();
    void copiesRawEffectiveSurfaceTitle();
    void reloadsMiddleClickClipboardPolicy();
    void routesConfiguredRightClickPolicy();
    void scalesAndAccumulatesDiscreteWheelInputAcrossReloads();
    void prefersPrecisionPixelsAndRetainsPhysicalWheelDistance();
    void normalizesHorizontalWheelInputIndependently();
    void switchesTabsFromPrecisionHorizontalScroll();
    void forwardsTypedSelectionPointerMetadataOnce();
    void invalidatesInspectorRequestDuringSynchronousDispatch();
    void isolatesInspectorCellPickGestures();
    void cancelsSelectionWhenMouseGrabIsRevoked();
    void togglesMouseReportingPolicyAcrossGesturesAndReloads();
    void appliesMouseShiftCaptureAcrossPointerRoutes();
    void routesAllPasteEntryPointsThroughController();
    void convertsTerminalDropContent();
    void routesTerminalDropsThroughPasteController();
    void routesUnsafePasteConfirmationThroughWorker();
    void reconcilesActivityAfterKamRejectsEnter();
    void hidesPointerOnlyForTerminalTypingAndRestoresOnInteraction();
    void focusesPaneAfterPhysicalPointerMotionAndReload();
    void asyncFallbackDoesNotHideAfterPointerActivity();
    void restoresHyperlinkPointerAfterTyping();
    void resolvesMinimumContrastWithShaderOrdering();
    void rendersBackgroundOpacityAndReloadsInPlace();
    void retainsBackgroundImageAcrossOptionReloadAndDecodeFailure();
    void rendersMinimumContrastAndReloadsLive();
    void runsCursorBlinkTimerOnlyWhenNeeded();
    void defaultCursorBlinkPublishesBothRenderPhases();
    void retainsMainTextRowsAcrossIncrementalUpdates();
    void invalidatesOnlyChangedSearchDecorationRows();
    void rendersAndRetainsKittyGraphics();
    void reloadsShapingAndTracksLogicalCursorRows();
    void retainsTextWhileDimmingUnfocusedSplits();
    void rebuildsMainTextRowsAfterWindowChange();
    void rendersConfiguredCellCursorAndDecorationAppearance();
    void rendersResolvedTypographyAndPhysicalGeometry();
    void clipsDecorationAndCursorSprites();
    void routesEmergencyTabShortcuts();
    void compiledProgramAvailabilityControlsEmergencyShortcuts();
    void forwardsConsumedShiftForLayoutText();
    void forwardsAuthoritativeLayoutMetadata();
    void remapsSidedModifiersAcrossBindingsAndTerminalInput();
    void routesConfiguredBindingsAndDisablesEmergencyFallback();
    void routesBroadConfiguredActionEffects();
    void routesTypedCloseTabModes();
    void routesViewportAndSelectionActions();
    void selectionActionPerformabilityUsesWorkerState_data();
    void selectionActionPerformabilityUsesWorkerState();
    void routesTerminalControlActions();
    void suspendsTerminalActionChainsUntilCorrelatedEffectsCommit();
    void cancelsPendingTerminalActionChainsBeforeSessionStart();
    void dropsQueuedTerminalEffectWhenShutdownBegins();
    void dropsPreExitSearchSelectionEffectOnSessionExit_data();
    void dropsPreExitSearchSelectionEffectOnSessionExit();
    void rejectsDuplicateTerminalActionRequestIds();
    void handlesCompletionReentrantlyDuringTerminalActionStart();
    void retainsWorkerPerformedStateWhenGuiEffectFails();
    void defersInputMethodDuringTerminalActionChains();
    void preservesDeferredInputFifoDuringReplayReentrancy();
    void survivesDestructionDuringDelayedActionFinalization();
    void dropsPendingConsumedKeyOwnershipOnFocusLoss();
    void routesTerminalFileActions();
    void dropsQueuedTerminalFileOpenAfterTeardown();
    void routesSearchActionsAndRetainsUiState();
    void interactsWithOsc8Hyperlinks();
    void interactsWithRegexLinksAndReloadsLinkUrl();
    void previewsLinksAccordingToPolicyAndBoundsDisplay();
    void keepsOsc8InteractionStableAcrossUnrelatedOutput();
    void restoresOsc8HoverAcrossViewportScroll();
    void letsShiftBypassMouseCaptureForHyperlinks();
    void resetPreservesSurfaceTitleAndClearsWorkingDirectory();
    void routesStructuredSequencesAndCancelsThemOnReload();
    void preservesStateWithinAKeybindProgramGeneration();
    void newerSameProgramRuntimeUpdateWinsReentry();
    void disabledMouseHideWinsSameProgramRuntimeReentry();
    void keyTableResetNotifiesBeforeLaterReentry();
    void runtimeOptionsObserverMayDestroyPane();
    void reloadsSafelyFromSequenceStagingNotification();
    void sequenceStagingObserverMayDestroyPane();
    void sequenceResolutionObserverMayDestroyPane();
    void reloadsSafelyFromOneShotLeaderTableNotification();
    void oneShotTableObserverCanContinueSequence();
    void reloadResolutionObserverUsesReplacementProgram();
    void oneShotLeafObserverCanStartSequence();
    void actionObserverCanStartIndependentSequence();
    void deferredUnconsumedKeysPreserveDispatchOrder();
    void deferredReleaseWaitsForConsumedPress();
    void replaysInvalidStructuredSequenceThroughPty();
    void routesNamedKeyTablesAndClearsThemOnReload();
    void rejectsMalformedFrontendActionsWithoutSideEffects();
};

void TerminalPaneTest::disabledCustomShadersAvoidOffscreenRenderLayers()
{
    LaunchOptions options;
    QVERIFY(options.customShaders.sources.isEmpty());

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);

    QVERIFY(pane.flags().testFlag(QQuickItem::ItemHasContents));
    for (QQuickItem *const child : pane.childItems()) {
        QVERIFY(!child->flags().testFlag(QQuickItem::ItemHasContents));
    }
    QVERIFY(pane.findChildren<TerminalCustomShaderEffect *>().isEmpty());
}

void TerminalPaneTest::delegatedCustomShaderRenderingTearsDownDirectPaintNode()
{
    LaunchOptions options;
    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);

    QVERIFY(terminalPaneDelegatedPaintNodeTeardownForTest(&pane));
    QVERIFY(pane.flags().testFlag(QQuickItem::ItemHasContents));
}

void TerminalPaneTest::routesWaitAfterCommandDismissalSeparatelyFromHold()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();

    {
        TerminalPane pane(options, nullptr, std::nullopt,
                          TerminalSessionStartMode::Deferred);
        auto *controller = pane.findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        QSignalSpy closed(&pane, &TerminalPane::requestClose);
        QSignalSpy ended(&pane, &TerminalPane::sessionEnded);

        Q_EMIT controller->sessionExited(0, 0, false, true, 500, false);
        QCOMPARE(ended.count(), 1);
        QCoreApplication::processEvents();
        QCOMPARE(closed.count(), 0);

        // The worker, not the pane, decides whether a key encoded bytes.
        // Until it publishes that one-shot result the waited pane stays open.
        QKeyEvent modifier(QEvent::KeyPress, Qt::Key_Control,
                           Qt::ControlModifier);
        QCoreApplication::sendEvent(&pane, &modifier);
        QCoreApplication::processEvents();
        QCOMPARE(closed.count(), 0);

        Q_EMIT controller->exitKeyDismissed();
        QCOMPARE(closed.count(), 1);
        Q_EMIT controller->exitKeyDismissed();
        QCOMPARE(closed.count(), 1);
    }

    {
        TerminalPane pane(options, nullptr, std::nullopt,
                          TerminalSessionStartMode::Deferred);
        auto *controller = pane.findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        QSignalSpy closed(&pane, &TerminalPane::requestClose);

        Q_EMIT controller->sessionExited(0, 0, true, false, 500, false);
        Q_EMIT controller->exitKeyDismissed();
        QCoreApplication::processEvents();
        QCOMPARE(closed.count(), 0);
    }

    {
        TerminalPane pane(options, nullptr, std::nullopt,
                          TerminalSessionStartMode::Deferred);
        auto *controller = pane.findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        QSignalSpy closed(&pane, &TerminalPane::requestClose);

        Q_EMIT controller->sessionExited(0, 0, false, false, 500, false);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 1000);
    }
}

void TerminalPaneTest::presentsAbnormalExitUntilExplicitDismissal()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();

    {
        TerminalPane pane(options, nullptr, std::nullopt,
                          TerminalSessionStartMode::Deferred);
        auto *controller = pane.findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        QSignalSpy abnormalChanged(&pane, &TerminalPane::abnormalExitChanged);
        QSignalSpy closed(&pane, &TerminalPane::requestClose);
        QSignalSpy ended(&pane, &TerminalPane::sessionEnded);

        Q_EMIT controller->sessionExited(7, 0, false, true, 73, true);
        QCOMPARE(ended.count(), 1);
        QCOMPARE(abnormalChanged.count(), 1);
        QVERIFY(pane.abnormalExitVisible());
        QCOMPARE(pane.abnormalExitText(),
                 QStringLiteral("Command failed after 73 ms (exit status 7)."));
        QCoreApplication::processEvents();
        QCOMPARE(closed.count(), 0);

        pane.dismissAbnormalExit();
        QCOMPARE(closed.count(), 1);
        // A workspace close confirmation may retain the pane. Keep the banner
        // available so the explicit request can be retried.
        QVERIFY(pane.abnormalExitVisible());
    }

    {
        TerminalPane pane(options, nullptr, std::nullopt,
                          TerminalSessionStartMode::Deferred);
        auto *controller = pane.findChild<TerminalController *>();
        QVERIFY(controller != nullptr);
        QSignalSpy closed(&pane, &TerminalPane::requestClose);

        Q_EMIT controller->sessionExited(143, 15, true, false, 11, true);
        QVERIFY(pane.abnormalExitVisible());
        QCOMPARE(pane.abnormalExitText(),
                 QStringLiteral("Command failed after 11 ms (signal 15)."));

        // The local --hold policy remains indefinite and does not become
        // key-dismissible merely because the outcome is abnormal.
        Q_EMIT controller->exitKeyDismissed();
        QCOMPARE(closed.count(), 0);
        pane.dismissAbnormalExit();
        QCOMPARE(closed.count(), 1);
    }
}

void TerminalPaneTest::presentsChildExecFailureEndToEnd()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/ghostty-qt-test/nonexistent-child"),
    };
    options.abnormalCommandExitRuntimeMilliseconds =
        std::numeric_limits<quint32>::max();

    TerminalPane pane(options);
    QSignalSpy abnormalChanged(&pane, &TerminalPane::abnormalExitChanged);
    QSignalSpy closed(&pane, &TerminalPane::requestClose);
    QSignalSpy ended(&pane, &TerminalPane::sessionEnded);

    QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 5000);
    QCOMPARE(ended.constFirst().at(1).toInt(), 127);
    QCOMPARE(ended.constFirst().at(2).toInt(), 0);
    QCOMPARE(abnormalChanged.count(), 1);
    QVERIFY(pane.abnormalExitVisible());
    QVERIFY(pane.abnormalExitText().startsWith(
        QStringLiteral("Command failed after ")));
    QCoreApplication::processEvents();
    QCOMPARE(closed.count(), 0);

    pane.dismissAbnormalExit();
    QCOMPARE(closed.count(), 1);
}

void TerminalPaneTest::presentsScrollbarFromRetainedMetadata()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy changed(&pane, &TerminalPane::scrollbarChanged);

    QVERIFY(!pane.scrollbarVisible());
    QCOMPARE(pane.scrollbarPosition(), 0.0);
    QCOMPARE(pane.scrollbarSize(), 1.0);

    controller->terminalUpdated(fullFrameWithScrollbar(100, 20, 25));
    QVERIFY(pane.scrollbarVisible());
    QVERIFY(std::abs(pane.scrollbarPosition() - 0.2) < 1e-12);
    QVERIFY(std::abs(pane.scrollbarSize() - 0.25) < 1e-12);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(pane.property("scrollbarVisible").toBool(), true);
    QCOMPARE(pane.property("scrollbarPosition").toReal(),
             pane.scrollbarPosition());
    QCOMPARE(pane.property("scrollbarSize").toReal(), pane.scrollbarSize());

    // Repeated authoritative metadata does not create a QML feedback cycle.
    controller->terminalUpdated(scrollbarMetadata(100, 20, 25, 2));
    QCOMPARE(changed.count(), 1);

    // A malformed offset is saturated to the last valid viewport position.
    controller->terminalUpdated(scrollbarMetadata(100, 500, 25, 3));
    QVERIFY(pane.scrollbarVisible());
    QVERIFY(std::abs(pane.scrollbarPosition() - 0.75) < 1e-12);
    QCOMPARE(changed.count(), 2);

    // No history, a zero viewport, and a viewport longer than the total are
    // all non-presentable states. Alternate-screen metadata naturally takes
    // the first path because its total and visible length are equal.
    controller->terminalUpdated(scrollbarMetadata(25, 0, 25, 4));
    QVERIFY(!pane.scrollbarVisible());
    QCOMPARE(pane.scrollbarPosition(), 0.0);
    QCOMPARE(pane.scrollbarSize(), 1.0);
    QCOMPARE(changed.count(), 3);
    controller->terminalUpdated(scrollbarMetadata(25, 0, 0, 5));
    controller->terminalUpdated(scrollbarMetadata(25, 0, 26, 6));
    QVERIFY(!pane.scrollbarVisible());
    QCOMPARE(changed.count(), 3);
}

void TerminalPaneTest::presentsProgressReportsAndAppliesLivePolicy()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.progressStyle = true;

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    auto *const controller = pane.findChild<TerminalController *>();
    auto *const timer =
        pane.findChild<QChronoTimer *>(QStringLiteral("progressReportTimer"));
    QVERIFY(controller != nullptr);
    QVERIFY(timer != nullptr);
    QVERIFY(timer->isSingleShot());
    QCOMPARE(timer->interval(), std::chrono::seconds(15));
    QSignalSpy changed(&pane, &TerminalPane::progressChanged);

    const auto publish = [controller](TerminalProgressState state,
                                      std::optional<quint8> progress = {}) {
        Q_EMIT controller->progressReportRequested({state, progress});
    };

    QVERIFY(!pane.progressVisible());
    QCOMPARE(pane.tabProgress(), -1);
    QCOMPARE(pane.progressActivityPosition(), qreal{0.0});
    QVERIFY(!timer->isActive());

    Q_EMIT controller->progressReportRequested({
        .state = TerminalProgressState::Indeterminate,
        .progress = std::nullopt,
        .activityPulses = quint8{20},
    });
    QVERIFY(pane.progressVisible());
    QVERIFY(pane.progressIndeterminate());
    QCOMPARE(pane.progressActivityPosition(), qreal{0.0});
    Q_EMIT controller->progressReportRequested({
        .state = TerminalProgressState::Indeterminate,
        .progress = std::nullopt,
        .activityPulses = quint8{12},
    });
    QCOMPARE(pane.progressActivityPosition(), qreal{0.8});
    Q_EMIT controller->progressReportRequested({
        .state = TerminalProgressState::Indeterminate,
        .progress = std::nullopt,
        .activityPulses = quint8{8},
    });
    QCOMPARE(pane.progressActivityPosition(), qreal{0.0});
    publish(TerminalProgressState::Remove);
    QVERIFY(!pane.progressVisible());
    changed.clear();

    publish(TerminalProgressState::Set, quint8{42});
    QVERIFY(pane.progressVisible());
    QCOMPARE(pane.progressValue(), 42);
    QVERIFY(!pane.progressIndeterminate());
    QVERIFY(!pane.progressError());
    QVERIFY(!pane.progressPaused());
    QCOMPARE(pane.tabProgress(), 42);
    QVERIFY(timer->isActive());
    QCOMPARE(changed.count(), 1);

    // An identical report only refreshes expiry; it does not churn QML.
    publish(TerminalProgressState::Set, quint8{42});
    QVERIFY(timer->isActive());
    QCOMPARE(changed.count(), 1);

    publish(TerminalProgressState::Error);
    QVERIFY(pane.progressVisible());
    QCOMPARE(pane.progressValue(), 42);
    QVERIFY(pane.progressIndeterminate());
    QVERIFY(pane.progressError());
    QVERIFY(!pane.progressPaused());
    QCOMPARE(pane.tabProgress(), -1);
    const qreal errorActivityPosition = pane.progressActivityPosition();
    QVERIFY(errorActivityPosition > 0.0);

    publish(TerminalProgressState::Pause);
    QCOMPARE(pane.progressValue(), 42);
    QVERIFY(pane.progressIndeterminate());
    QVERIFY(pane.progressError());
    QVERIFY(pane.progressPaused());
    QCOMPARE(pane.tabProgress(), -1);
    QCOMPARE(pane.progressActivityPosition(), errorActivityPosition);

    publish(TerminalProgressState::Pause, quint8{75});
    QCOMPARE(pane.progressValue(), 75);
    QVERIFY(!pane.progressIndeterminate());
    QVERIFY(pane.progressError());
    QVERIFY(pane.progressPaused());
    QCOMPARE(pane.tabProgress(), 75);

    publish(TerminalProgressState::Set, quint8{80});
    QCOMPARE(pane.progressValue(), 80);
    QVERIFY(!pane.progressIndeterminate());
    QVERIFY(!pane.progressError());
    QVERIFY(!pane.progressPaused());
    QCOMPARE(pane.tabProgress(), 80);

    const int changesBeforeActivity = changed.count();
    const qreal activityBefore = pane.progressActivityPosition();
    publish(TerminalProgressState::Indeterminate);
    QVERIFY(pane.progressIndeterminate());
    QVERIFY(!pane.progressError());
    QCOMPARE(pane.tabProgress(), -1);
    QVERIFY(pane.progressActivityPosition() != activityBefore);
    QCOMPARE(changed.count(), changesBeforeActivity + 1);
    const qreal firstPulse = pane.progressActivityPosition();
    publish(TerminalProgressState::Indeterminate);
    QVERIFY(pane.progressActivityPosition() != firstPulse);
    QCOMPARE(changed.count(), changesBeforeActivity + 2);

    publish(TerminalProgressState::Remove);
    QVERIFY(!pane.progressVisible());
    QVERIFY(!pane.progressPaused());
    QVERIFY(!timer->isActive());
    QCOMPARE(pane.tabProgress(), -1);

    // GTK retains hidden presentation internals. A later pause without a
    // percentage can intentionally reuse them, but a policy re-enable alone
    // cannot resurrect the bar.
    publish(TerminalProgressState::Pause);
    QVERIFY(pane.progressVisible());
    QVERIFY(pane.progressIndeterminate());
    QVERIFY(pane.progressPaused());

    LaunchOptions disabled = options;
    disabled.progressStyle = false;
    pane.applyRuntimeOptions(disabled);
    QVERIFY(!pane.progressVisible());
    QVERIFY(!timer->isActive());
    publish(TerminalProgressState::Set, quint8{5});
    QVERIFY(!pane.progressVisible());
    QCOMPARE(pane.progressValue(), 80);

    pane.applyRuntimeOptions(options);
    QVERIFY(!pane.progressVisible());
    publish(TerminalProgressState::Set, quint8{90});
    QVERIFY(pane.progressVisible());
    QCOMPARE(pane.tabProgress(), 90);
    QVERIFY(timer->isActive());

    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QVERIFY(!pane.progressVisible());
    QVERIFY(!timer->isActive());
    QCOMPARE(pane.tabProgress(), -1);

    pane.beginShutdown();
    publish(TerminalProgressState::Set, quint8{100});
    QVERIFY(!pane.progressVisible());
    QVERIFY(!timer->isActive());
}

void TerminalPaneTest::movesScrollbarWithClampedAbsoluteRows()
{
    qRegisterMetaType<TerminalViewportRequest>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy requests(controller, &TerminalController::scrollRequested);
    controller->terminalUpdated(fullFrameWithScrollbar(100, 20, 20));

    const auto requestAt = [&requests](int index) {
        return qvariant_cast<TerminalViewportRequest>(
            requests.at(index).constFirst());
    };

    pane.scrollbarMoveTo(0.2);
    pane.scrollbarMoveTo(0.204);
    QCOMPARE(requests.count(), 0);

    pane.scrollbarMoveTo(0.5);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requestAt(0).kind, TerminalViewportRequest::Kind::Row);
    QCOMPARE(requestAt(0).row, quint64{50});
    pane.scrollbarMoveTo(0.5);
    QCOMPARE(requests.count(), 1);

    // Returning to the authoritative row before the worker acknowledges the
    // first request must supersede it instead of letting the stale row win.
    pane.scrollbarMoveTo(0.2);
    QCOMPARE(requests.count(), 2);
    QCOMPARE(requestAt(1).kind, TerminalViewportRequest::Kind::Row);
    QCOMPARE(requestAt(1).row, quint64{20});
    pane.scrollbarMoveTo(0.2);
    QCOMPARE(requests.count(), 2);

    pane.scrollbarMoveTo(0.51);
    QCOMPARE(requests.count(), 3);
    QCOMPARE(requestAt(2).kind, TerminalViewportRequest::Kind::Row);
    QCOMPARE(requestAt(2).row, quint64{51});
    pane.scrollbarMoveTo(-std::numeric_limits<qreal>::infinity());
    QCOMPARE(requests.count(), 4);
    QCOMPARE(requestAt(3).kind, TerminalViewportRequest::Kind::Row);
    QCOMPARE(requestAt(3).row, quint64{0});
    pane.scrollbarMoveTo(std::numeric_limits<qreal>::infinity());
    QCOMPARE(requests.count(), 5);
    QCOMPARE(requestAt(4).kind, TerminalViewportRequest::Kind::Row);
    QCOMPARE(requestAt(4).row, quint64{80});
    pane.scrollbarMoveTo(std::numeric_limits<qreal>::quiet_NaN());
    QCOMPARE(requests.count(), 5);

    // Once the worker publishes the requested viewport, a bound ScrollBar
    // writing the same normalized position back is a no-op.
    controller->terminalUpdated(scrollbarMetadata(100, 80, 20, 2));
    pane.scrollbarMoveTo(1.0);
    QCOMPARE(requests.count(), 5);
}

void TerminalPaneTest::reloadsScrollbarPolicyAndHandlesHugeCounts()
{
    qRegisterMetaType<TerminalViewportRequest>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    pane.setSize(QSizeF(640.0, 480.0));
    TerminalPane *const identity = &pane;
    const QSizeF geometry = pane.size();
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy requests(controller, &TerminalController::scrollRequested);
    controller->terminalUpdated(fullFrameWithScrollbar(200, 100, 50));
    QVERIFY(pane.scrollbarVisible());

    LaunchOptions hidden = options;
    hidden.scrollbar = ScrollbarPolicy::Never;
    pane.applyRuntimeOptions(hidden);
    QCOMPARE(&pane, identity);
    QCOMPARE(pane.size(), geometry);
    QVERIFY(!pane.scrollbarVisible());
    QCOMPARE(pane.scrollbarPosition(), 0.0);
    QCOMPARE(pane.scrollbarSize(), 1.0);
    pane.scrollbarMoveTo(0.25);
    QCOMPARE(requests.count(), 0);

    // Metadata continues to update while the frontend control is disabled,
    // then becomes authoritative as soon as policy is re-enabled.
    controller->terminalUpdated(scrollbarMetadata(400, 120, 100, 2));
    QVERIFY(!pane.scrollbarVisible());
    pane.applyRuntimeOptions(options);
    QVERIFY(pane.scrollbarVisible());
    QVERIFY(std::abs(pane.scrollbarPosition() - 0.3) < 1e-12);
    QVERIFY(std::abs(pane.scrollbarSize() - 0.25) < 1e-12);
    QCOMPARE(&pane, identity);
    QCOMPARE(pane.size(), geometry);

    constexpr quint64 maximum = std::numeric_limits<quint64>::max();
    constexpr quint64 length = maximum / 4;
    constexpr quint64 maximumOffset = maximum - length;
    controller->terminalUpdated(scrollbarMetadata(maximum, maximum, length, 3));
    QVERIFY(pane.scrollbarVisible());
    QVERIFY(std::isfinite(pane.scrollbarPosition()));
    QVERIFY(std::isfinite(pane.scrollbarSize()));
    QVERIFY(pane.scrollbarPosition() >= 0.0);
    QVERIFY(pane.scrollbarPosition() <= 1.0);
    QVERIFY(pane.scrollbarSize() > 0.0);
    QVERIFY(pane.scrollbarSize() < 1.0);
    const qreal expectedPosition =
        static_cast<qreal>(static_cast<long double>(maximumOffset)
                           / static_cast<long double>(maximum));
    const qreal expectedSize = static_cast<qreal>(
        static_cast<long double>(length) / static_cast<long double>(maximum));
    QCOMPARE(pane.scrollbarPosition(), expectedPosition);
    QCOMPARE(pane.scrollbarSize(), expectedSize);

    // The malformed current offset is clamped to the bottom, so its bound
    // value is a no-op. A different normalized position converts in C++
    // without overflowing or passing a quint64 through QML.
    pane.scrollbarMoveTo(1.0);
    QCOMPARE(requests.count(), 0);
    pane.scrollbarMoveTo(0.25);
    QCOMPARE(requests.count(), 1);
    const TerminalViewportRequest request =
        qvariant_cast<TerminalViewportRequest>(
            requests.constFirst().constFirst());
    QCOMPARE(request.kind, TerminalViewportRequest::Kind::Row);
    QCOMPARE(request.row, quint64{1} << 62);

    controller->terminalUpdated(scrollbarMetadata(maximum, 0, 0, 4));
    QVERIFY(!pane.scrollbarVisible());
    pane.scrollbarMoveTo(0.5);
    QCOMPARE(requests.count(), 1);
}

void TerminalPaneTest::latchesClearsAndReloadsBellFeedback()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.bellFeatures = {
        .system = false,
        .audio = false,
        .attention = true,
        .title = true,
        .border = false,
    };

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    const QString baseTitle = QStringLiteral("  base 👻  ");
    const QString surfaceTitle = QStringLiteral("  surface 🌐  ");
    pane.setSurfaceTitle(baseTitle);
    pane.setSurfaceTitleOverride(std::optional<QString>{surfaceTitle});
    QCOMPARE(pane.title(), surfaceTitle);
    QCOMPARE(pane.effectiveSurfaceTitle(),
             std::optional<QString>{surfaceTitle});

    QSignalSpy changed(&pane, &TerminalPane::bellChanged);
    QSignalSpy titleChanged(&pane, &TerminalPane::titleChanged);
    QSignalSpy runtimeOptions(controller,
                              &TerminalController::runtimeOptionsRequested);
    int ringCount = 0;
    QPointer<TerminalPane> ringingPane;
    connect(&pane, &TerminalPane::bellRang, &pane, [&](TerminalPane *source) {
        ++ringCount;
        ringingPane = source;
    });

    const auto verifyRawTitle = [&] {
        QCOMPARE(pane.title(), surfaceTitle);
        QCOMPARE(pane.effectiveSurfaceTitle(),
                 std::optional<QString>{surfaceTitle});
        QCOMPARE(pane.surfaceTitleOverride(),
                 std::optional<QString>{surfaceTitle});
        QCOMPARE(titleChanged.count(), 0);
    };
    const auto verifyLastRing = [&] {
        QVERIFY(ringCount > 0);
        QCOMPARE(ringingPane.data(), &pane);
    };

    // Every BEL publishes its per-event effects. Only the first transition
    // changes the retained pane state.
    Q_EMIT controller->bell();
    QVERIFY(pane.bellRinging());
    QVERIFY(pane.bellTitleVisible());
    QVERIFY(!pane.bellBorderVisible());
    QCOMPARE(changed.count(), 1);
    QCOMPARE(ringCount, 1);
    verifyLastRing();
    verifyRawTitle();

    Q_EMIT controller->bell();
    QVERIFY(pane.bellRinging());
    QCOMPARE(changed.count(), 1);
    QCOMPARE(ringCount, 2);
    verifyLastRing();
    verifyRawTitle();

    // Presentation flags reload around the existing latch. They neither
    // mutate the raw title layers nor project frontend-only state to the
    // terminal worker.
    LaunchOptions borderOnly = options;
    borderOnly.bellFeatures.attention = false;
    borderOnly.bellFeatures.title = false;
    borderOnly.bellFeatures.border = true;
    pane.applyRuntimeOptions(borderOnly);
    QVERIFY(pane.bellRinging());
    QVERIFY(!pane.bellTitleVisible());
    QVERIFY(pane.bellBorderVisible());
    QCOMPARE(changed.count(), 2);
    QCOMPARE(runtimeOptions.count(), 0);
    verifyRawTitle();

    Q_EMIT controller->bell();
    QCOMPARE(changed.count(), 2);
    QCOMPARE(ringCount, 3);
    verifyLastRing();

    // Reapplying an identical generation is inert.
    pane.applyRuntimeOptions(borderOnly);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(runtimeOptions.count(), 0);

    LaunchOptions bothVisible = borderOnly;
    bothVisible.bellFeatures.attention = true;
    bothVisible.bellFeatures.title = true;
    pane.applyRuntimeOptions(bothVisible);
    QVERIFY(pane.bellRinging());
    QVERIFY(pane.bellTitleVisible());
    QVERIFY(pane.bellBorderVisible());
    QCOMPARE(changed.count(), 3);
    QCOMPARE(runtimeOptions.count(), 0);
    verifyRawTitle();

    // Modifier presses alone do not acknowledge the alert.
    for (const int key : {
             Qt::Key_Control,
             Qt::Key_Shift,
             Qt::Key_Alt,
             Qt::Key_AltGr,
             Qt::Key_Meta,
         }) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(&pane, &press);
        QVERIFY(pane.bellRinging());
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QCoreApplication::sendEvent(&pane, &release);
        QVERIFY(pane.bellRinging());
    }
    QCOMPARE(changed.count(), 3);

    QKeyEvent ordinaryPress(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier,
                            QStringLiteral("x"));
    QCoreApplication::sendEvent(&pane, &ordinaryPress);
    QVERIFY(!pane.bellRinging());
    QVERIFY(!pane.bellTitleVisible());
    QVERIFY(!pane.bellBorderVisible());
    QCOMPARE(changed.count(), 4);
    QKeyEvent ordinaryRelease(QEvent::KeyRelease, Qt::Key_X, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &ordinaryRelease);
    QCOMPARE(changed.count(), 4);
    verifyRawTitle();

    // A pointer press clears before focus handling, so this remains valid for
    // an already-focused pane where no focus transition would help.
    Q_EMIT controller->bell();
    QCOMPARE(changed.count(), 5);
    QCOMPARE(ringCount, 4);
    const QPointF pointer(4.0, 4.0);
    QMouseEvent mousePress(QEvent::MouseButtonPress, pointer, pointer, pointer,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &mousePress);
    QVERIFY(!pane.bellRinging());
    QCOMPARE(changed.count(), 6);
    QMouseEvent mouseRelease(QEvent::MouseButtonRelease, pointer, pointer,
                             pointer, Qt::LeftButton, Qt::NoButton,
                             Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &mouseRelease);
    QCOMPARE(changed.count(), 6);

    // A null IME callback is not interaction, while either committed text or
    // a real preedit transition acknowledges the bell.
    Q_EMIT controller->bell();
    QCOMPARE(changed.count(), 7);
    QCOMPARE(ringCount, 5);
    QInputMethodEvent emptyInput;
    QCoreApplication::sendEvent(&pane, &emptyInput);
    QVERIFY(pane.bellRinging());
    QCOMPARE(changed.count(), 7);

    QInputMethodEvent committedInput;
    committedInput.setCommitString(QStringLiteral("é"));
    QCoreApplication::sendEvent(&pane, &committedInput);
    QVERIFY(!pane.bellRinging());
    QCOMPARE(changed.count(), 8);

    Q_EMIT controller->bell();
    QCOMPARE(changed.count(), 9);
    QCOMPARE(ringCount, 6);
    QInputMethodEvent preeditInput(QStringLiteral("compose"), {});
    QCoreApplication::sendEvent(&pane, &preeditInput);
    QVERIFY(!pane.bellRinging());
    QCOMPARE(changed.count(), 10);

    // Focus loss is inert; the next focus gain clears the retained alert.
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(&pane, &focusOut);
    Q_EMIT controller->bell();
    QVERIFY(pane.bellRinging());
    QCOMPARE(changed.count(), 11);
    QCOMPARE(ringCount, 7);
    QFocusEvent focusIn(QEvent::FocusIn, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(&pane, &focusIn);
    QVERIFY(!pane.bellRinging());
    QCOMPARE(changed.count(), 12);
    verifyRawTitle();
}

void TerminalPaneTest::routesAudibleBellEffectsForEveryEventAndReload()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.bellFeatures = {
        .system = false,
        .audio = false,
        .attention = false,
        .title = false,
        .border = false,
    };

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    PaneBellDeviceState state;
    pane.setBellPlaybackDevice(std::make_unique<FakePaneBellDevice>(state));
    QSignalSpy runtimeOptions(controller,
                              &TerminalController::runtimeOptionsRequested);

    Q_EMIT controller->bell();
    QVERIFY(pane.bellRinging());
    QCOMPARE(state.systemBells, 0);
    QVERIFY(state.sources.isEmpty());

    LaunchOptions system = options;
    system.bellFeatures.system = true;
    pane.applyRuntimeOptions(system);
    Q_EMIT controller->bell();
    Q_EMIT controller->bell();
    QCOMPARE(state.systemBells, 2);
    QVERIFY(state.sources.isEmpty());

    const GhosttyConfigPath required{
        .path = QStringLiteral("/tmp/ghostty-qt-required-bell.ogg"),
        .optional = false,
    };
    LaunchOptions audio = system;
    audio.bellFeatures.system = false;
    audio.bellFeatures.audio = true;
    audio.bellAudioPath = required;
    audio.bellAudioVolume = -2.0;
    pane.applyRuntimeOptions(audio);
    Q_EMIT controller->bell();
    Q_EMIT controller->bell();
    QCOMPARE(state.systemBells, 2);
    QCOMPARE(state.sources.size(), 1);
    QVERIFY(state.sources.constFirst() == required);
    QCOMPARE(state.volumes, QList<double>({0.0, 0.0}));
    QCOMPARE(state.restarts, 2);

    // Required/optional provenance is part of the cached source identity.
    // Volume and path reloads affect only future bells and stay GUI-local.
    const GhosttyConfigPath optional{
        .path = required.path,
        .optional = true,
    };
    LaunchOptions reloaded = audio;
    reloaded.bellAudioPath = optional;
    reloaded.bellAudioVolume = 4.0;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(state.sources.size(), 1);
    QCOMPARE(state.restarts, 2);
    Q_EMIT controller->bell();
    QCOMPARE(state.sources.size(), 2);
    QVERIFY(state.sources.constLast() == optional);
    QCOMPARE(state.volumes.constLast(), 1.0);
    QCOMPARE(state.restarts, 3);

    LaunchOptions missing = reloaded;
    missing.bellAudioPath.reset();
    pane.applyRuntimeOptions(missing);
    Q_EMIT controller->bell();
    QCOMPARE(state.sources.size(), 2);
    QCOMPARE(state.restarts, 3);

    LaunchOptions both = missing;
    both.bellFeatures.system = true;
    both.bellAudioPath = optional;
    both.bellAudioVolume = 0.75;
    pane.applyRuntimeOptions(both);
    Q_EMIT controller->bell();
    QCOMPARE(state.systemBells, 3);
    // The cached optional source survives a temporarily absent path.
    QCOMPARE(state.sources.size(), 2);
    QCOMPARE(state.volumes.constLast(), 0.75);
    QCOMPARE(state.restarts, 4);
    QCOMPARE(runtimeOptions.count(), 0);
}

void TerminalPaneTest::audibleBellLeaseSurvivesDestructiveObserver()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.bellFeatures = {
        .system = true,
        .audio = true,
        .attention = false,
        .title = false,
        .border = false,
    };
    options.bellAudioPath = GhosttyConfigPath{
        .path = QStringLiteral("/tmp/destructive-observer-bell.oga"),
        .optional = true,
    };
    options.bellAudioVolume = 0.4;

    auto *pane = new TerminalPane(options, nullptr, std::nullopt,
                                  TerminalSessionStartMode::Deferred);
    QPointer<TerminalPane> paneGuard(pane);
    QPointer<TerminalController> controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    PaneBellDeviceState state;
    pane->setBellPlaybackDevice(std::make_unique<FakePaneBellDevice>(state));
    connect(pane, &TerminalPane::bellRang, pane,
            [pane](TerminalPane *) { delete pane; });

    Q_EMIT controller->bell();
    QVERIFY(paneGuard == nullptr);
    QVERIFY(controller == nullptr);
    QCOMPARE(state.systemBells, 1);
    QCOMPARE(state.sources.size(), 1);
    QCOMPARE(state.volumes, QList<double>{0.4});
    QCOMPARE(state.restarts, 1);
}

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
    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography);
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

void TerminalPaneTest::defaultCursorBlinkPublishesBothRenderPhases()
{
    static const int shaderPipelineQmlType =
        qmlRegisterType<TerminalCustomShaderPipelineEffect>(
            "GhosttyQtPaneTest", 1, 0, "TerminalCustomShaderPipelineEffect");
    QVERIFY(shaderPipelineQmlType >= 0);

    QQmlEngine engine;
    QQmlComponent shaderStage(&engine);
    shaderStage.setData(
        R"qml(
import QtQuick
import GhosttyQtPaneTest 1.0

Item {
    id: root
    required property bool retainedPipeline
    required property var uniformProvider
    required property real sourceDevicePixelRatio
    required property bool linearBlending
    property var shaderStages: []

    layer.enabled: true
    layer.live: true
    layer.smooth: true
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.format: linearBlending ? ShaderEffectSource.RGBA16F
                                 : ShaderEffectSource.RGBA8
    layer.textureSize: Qt.size(
        Math.max(1, Math.round(width * sourceDevicePixelRatio)),
        Math.max(1, Math.round(height * sourceDevicePixelRatio)))
    layer.effect: Component {
        TerminalCustomShaderPipelineEffect {
            shaderStages: root.shaderStages
            uniformProvider: root.uniformProvider
            linearBlending: root.linearBlending
        }
    }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/terminal-pane-shader-stage.qml")));
    QVERIFY2(shaderStage.isReady(), qPrintable(shaderStage.errorString()));

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 5"),
    };
    options.hold = true;
    useSystemFixedFont(options);
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;
    options.appearance.cursorColor =
        TerminalColorValue::fromColor(QColor(QStringLiteral("#ff00ff")));
    QVERIFY(!options.appearance.cursorBlink.has_value());

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(320, 160);
    auto *workspace = new TerminalWorkspace(window.contentItem());
    workspace->setSize(window.size());
    workspace->setCustomShaderStageComponent(&shaderStage);
    QVERIFY(workspace->initialize(options));
    auto *pane = workspace->findChild<TerminalPane *>();
    QVERIFY(pane != nullptr);
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    auto *cursorTimer =
        pane->findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
    QVERIFY(cursorTimer != nullptr);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->sessionStarted(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(cursorTimer->isActive(), 3000);

    const auto presentedCursorVisible = [&window] {
        const QImage image = window.grabWindow();
        if (image.isNull()) return false;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.red() >= 220 && pixel.green() <= 40
                    && pixel.blue() >= 220) {
                    return true;
                }
            }
        }
        return false;
    };

    // A scene-graph probe only proves that updatePaintNode constructed the
    // next cursor geometry. Verify repeated presented frames as well: a
    // retained hardware batch must recover after its vertex count reaches
    // zero during every hidden phase.
    QTRY_VERIFY_WITH_TIMEOUT(presentedCursorVisible(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!presentedCursorVisible(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(presentedCursorVisible(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!presentedCursorVisible(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(presentedCursorVisible(), 3000);

    window.close();
}

void TerminalPaneTest::retainsMainTextRowsAcrossIncrementalUpdates()
{
    qRegisterMetaType<TerminalSearchUpdate>();
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    useSystemFixedFont(options);
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

    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography);
    const qreal cellWidth = metrics.cellWidth;
    const qreal cellHeight = metrics.cellHeight;

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
        update.cells[1].background = color;
        update.cells[1].backgroundExplicit = true;
        update.cells[2].overline = true;
        if (withGlyph) {
            update.cells[0].text = QString(QChar(0x2588));
            update.cells[3].strikeThrough = true;
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
    QCOMPARE(initial.rowSolidBuildCounts.size(), rows);
    for (quint64 count : initial.rowBuildCounts) {
        QVERIFY(count > 0);
    }
    for (quint64 count : initial.rowSolidBuildCounts) {
        QVERIFY(count > 0);
    }
    if (initial.retainedSolidRowGeometry) {
        QCOMPARE(initial.rowBackgroundGeometryCommitCounts.size(), rows);
        QCOMPARE(initial.rowDecorationBeforeTextGeometryCommitCounts.size(),
                 rows);
        QCOMPARE(initial.rowDecorationAfterTextGeometryCommitCounts.size(),
                 rows);
        for (quint64 count : initial.rowBackgroundGeometryCommitCounts) {
            QVERIFY(count > 0);
        }
        for (quint64 count :
             initial.rowDecorationBeforeTextGeometryCommitCounts) {
            QVERIFY(count > 0);
        }
        for (quint64 count :
             initial.rowDecorationAfterTextGeometryCommitCounts) {
            QVERIFY(count > 0);
        }
    } else {
        QVERIFY(initial.rowBackgroundGeometryCommitCounts.isEmpty());
        QVERIFY(initial.rowDecorationBeforeTextGeometryCommitCounts.isEmpty());
        QVERIFY(initial.rowDecorationAfterTextGeometryCommitCounts.isEmpty());
        QVERIFY(initial.globalBackgroundGeometryCommitCount > 0);
        QVERIFY(initial.globalDecorationBeforeTextGeometryCommitCount > 0);
        QVERIFY(initial.globalDecorationAfterTextGeometryCommitCount > 0);
    }
    QVERIFY(initial.kittyBelowBackgroundNodeSerial != 0);
    QVERIFY(initial.kittyBelowTextNodeSerial != 0);
    QVERIFY(initial.kittyAboveTextNodeSerial != 0);
    const QVector<TerminalPaneRenderLayer> expectedLayerOrder{
        TerminalPaneRenderLayer::KittyBelowBackground,
        TerminalPaneRenderLayer::CellBackground,
        TerminalPaneRenderLayer::KittyBelowText,
        TerminalPaneRenderLayer::CursorBackground,
        TerminalPaneRenderLayer::CellForeground,
        TerminalPaneRenderLayer::KittyAboveText,
        TerminalPaneRenderLayer::TerminalOverlay,
    };
    QVERIFY(initial.renderLayerOrder == expectedLayerOrder);

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
    QCOMPARE(metadata.rowSolidBuildCounts, initial.rowSolidBuildCounts);
    QCOMPARE(metadata.rowBackgroundGeometryCommitCounts,
             initial.rowBackgroundGeometryCommitCounts);
    QCOMPARE(metadata.rowDecorationBeforeTextGeometryCommitCounts,
             initial.rowDecorationBeforeTextGeometryCommitCounts);
    QCOMPARE(metadata.rowDecorationAfterTextGeometryCommitCounts,
             initial.rowDecorationAfterTextGeometryCommitCounts);
    QCOMPARE(metadata.globalBackgroundGeometryCommitCount,
             initial.globalBackgroundGeometryCommitCount);
    QCOMPARE(metadata.globalDecorationBeforeTextGeometryCommitCount,
             initial.globalDecorationBeforeTextGeometryCommitCount);
    QCOMPARE(metadata.globalDecorationAfterTextGeometryCommitCount,
             initial.globalDecorationAfterTextGeometryCommitCount);
    QCOMPARE(metadata.solidCellVisitCount, initial.solidCellVisitCount);
    QCOMPARE(metadata.kittyBelowBackgroundNodeSerial,
             initial.kittyBelowBackgroundNodeSerial);
    QCOMPARE(metadata.kittyBelowTextNodeSerial,
             initial.kittyBelowTextNodeSerial);
    QCOMPARE(metadata.kittyAboveTextNodeSerial,
             initial.kittyAboveTextNodeSerial);
    QCOMPARE(metadata.renderLayerOrder, initial.renderLayerOrder);

    // A pixel resize that leaves grid rows, columns, and snapped cell metrics
    // unchanged updates text-node clipping without reshaping every row.
    pane->setWidth(pane->width() + 1.0);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot pixelResized =
        terminalPaneRenderProbe(pane);
    QCOMPARE(pixelResized.rootSerial, metadata.rootSerial);
    QCOMPARE(pixelResized.rowNodeSerials, metadata.rowNodeSerials);
    QCOMPARE(pixelResized.rowBuildCounts, metadata.rowBuildCounts);
    QCOMPARE(pixelResized.rowSolidBuildCounts, metadata.rowSolidBuildCounts);
    QCOMPARE(pixelResized.rowBackgroundGeometryCommitCounts,
             metadata.rowBackgroundGeometryCommitCounts);
    QCOMPARE(pixelResized.rowDecorationBeforeTextGeometryCommitCounts,
             metadata.rowDecorationBeforeTextGeometryCommitCounts);
    QCOMPARE(pixelResized.rowDecorationAfterTextGeometryCommitCounts,
             metadata.rowDecorationAfterTextGeometryCommitCounts);
    QCOMPARE(pixelResized.globalBackgroundGeometryCommitCount,
             metadata.globalBackgroundGeometryCommitCount);
    QCOMPARE(pixelResized.globalDecorationBeforeTextGeometryCommitCount,
             metadata.globalDecorationBeforeTextGeometryCommitCount);
    QCOMPARE(pixelResized.globalDecorationAfterTextGeometryCommitCount,
             metadata.globalDecorationAfterTextGeometryCommitCount);
    QCOMPARE(pixelResized.solidCellVisitCount, metadata.solidCellVisitCount);

    // GUI-only overlays rebuild transient layers without touching main text.
    controller->errorOccurred(QStringLiteral("renderer overlay update"));
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot overlay =
        terminalPaneRenderProbe(pane);
    QCOMPARE(overlay.rootSerial, pixelResized.rootSerial);
    QCOMPARE(overlay.rowNodeSerials, pixelResized.rowNodeSerials);
    QCOMPARE(overlay.rowBuildCounts, pixelResized.rowBuildCounts);
    QCOMPARE(overlay.rowSolidBuildCounts, pixelResized.rowSolidBuildCounts);
    QCOMPARE(overlay.rowBackgroundGeometryCommitCounts,
             pixelResized.rowBackgroundGeometryCommitCounts);
    QCOMPARE(overlay.rowDecorationBeforeTextGeometryCommitCounts,
             pixelResized.rowDecorationBeforeTextGeometryCommitCounts);
    QCOMPARE(overlay.rowDecorationAfterTextGeometryCommitCounts,
             pixelResized.rowDecorationAfterTextGeometryCommitCounts);
    QCOMPARE(overlay.globalBackgroundGeometryCommitCount,
             pixelResized.globalBackgroundGeometryCommitCount);
    QCOMPARE(overlay.globalDecorationBeforeTextGeometryCommitCount,
             pixelResized.globalDecorationBeforeTextGeometryCommitCount);
    QCOMPARE(overlay.globalDecorationAfterTextGeometryCommitCount,
             pixelResized.globalDecorationAfterTextGeometryCommitCount);
    QCOMPARE(overlay.solidCellVisitCount, pixelResized.solidCellVisitCount);
    QVERIFY(overlay.paneOverlayTextNodeSerial != 0);
    QVERIFY(overlay.paneOverlayTextBuildCount > 0);

    pane->update();
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot repeatedOverlay =
        terminalPaneRenderProbe(pane);
    QCOMPARE(repeatedOverlay.paneOverlayTextNodeSerial,
             overlay.paneOverlayTextNodeSerial);
    QCOMPARE(repeatedOverlay.paneOverlayTextBuildCount,
             overlay.paneOverlayTextBuildCount);
    QCOMPARE(repeatedOverlay.rowNodeSerials, overlay.rowNodeSerials);
    QCOMPARE(repeatedOverlay.rowBuildCounts, overlay.rowBuildCounts);
    QCOMPARE(repeatedOverlay.rowSolidBuildCounts, overlay.rowSolidBuildCounts);
    QCOMPARE(repeatedOverlay.rowBackgroundGeometryCommitCounts,
             overlay.rowBackgroundGeometryCommitCounts);
    QCOMPARE(repeatedOverlay.rowDecorationBeforeTextGeometryCommitCounts,
             overlay.rowDecorationBeforeTextGeometryCommitCounts);
    QCOMPARE(repeatedOverlay.rowDecorationAfterTextGeometryCommitCounts,
             overlay.rowDecorationAfterTextGeometryCommitCounts);
    QCOMPARE(repeatedOverlay.globalBackgroundGeometryCommitCount,
             overlay.globalBackgroundGeometryCommitCount);
    QCOMPARE(repeatedOverlay.globalDecorationBeforeTextGeometryCommitCount,
             overlay.globalDecorationBeforeTextGeometryCommitCount);
    QCOMPARE(repeatedOverlay.globalDecorationAfterTextGeometryCommitCount,
             overlay.globalDecorationAfterTextGeometryCommitCount);
    QCOMPARE(repeatedOverlay.solidCellVisitCount, overlay.solidCellVisitCount);

    controller->errorOccurred(QStringLiteral("updated renderer overlay"));
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot updatedOverlay =
        terminalPaneRenderProbe(pane);
    QCOMPARE(updatedOverlay.paneOverlayTextNodeSerial,
             overlay.paneOverlayTextNodeSerial);
    QCOMPARE(updatedOverlay.paneOverlayTextBuildCount,
             overlay.paneOverlayTextBuildCount + 1);
    QCOMPARE(updatedOverlay.rowNodeSerials, overlay.rowNodeSerials);
    QCOMPARE(updatedOverlay.rowBuildCounts, overlay.rowBuildCounts);
    QCOMPARE(updatedOverlay.rowSolidBuildCounts, overlay.rowSolidBuildCounts);
    QCOMPARE(updatedOverlay.rowBackgroundGeometryCommitCounts,
             overlay.rowBackgroundGeometryCommitCounts);
    QCOMPARE(updatedOverlay.rowDecorationBeforeTextGeometryCommitCounts,
             overlay.rowDecorationBeforeTextGeometryCommitCounts);
    QCOMPARE(updatedOverlay.rowDecorationAfterTextGeometryCommitCounts,
             overlay.rowDecorationAfterTextGeometryCommitCounts);
    QCOMPARE(updatedOverlay.globalBackgroundGeometryCommitCount,
             overlay.globalBackgroundGeometryCommitCount);
    QCOMPARE(updatedOverlay.globalDecorationBeforeTextGeometryCommitCount,
             overlay.globalDecorationBeforeTextGeometryCommitCount);
    QCOMPARE(updatedOverlay.globalDecorationAfterTextGeometryCommitCount,
             overlay.globalDecorationAfterTextGeometryCommitCount);
    QCOMPARE(updatedOverlay.solidCellVisitCount, overlay.solidCellVisitCount);

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
    QVector<quint64> sparseSolidBuildCounts =
        updatedOverlay.rowSolidBuildCounts;
    ++sparseSolidBuildCounts[0];
    ++sparseSolidBuildCounts[2];
    QCOMPARE(sparse.rowSolidBuildCounts, sparseSolidBuildCounts);
    QCOMPARE(sparse.solidCellVisitCount,
             updatedOverlay.solidCellVisitCount
                 + static_cast<quint64>(2 * columns));
    if (sparse.retainedSolidRowGeometry) {
        QVector<quint64> sparseBackgroundCommits =
            updatedOverlay.rowBackgroundGeometryCommitCounts;
        ++sparseBackgroundCommits[0];
        QCOMPARE(sparse.rowBackgroundGeometryCommitCounts,
                 sparseBackgroundCommits);
        QVector<quint64> sparseBeforeTextCommits =
            updatedOverlay.rowDecorationBeforeTextGeometryCommitCounts;
        ++sparseBeforeTextCommits[0];
        QCOMPARE(sparse.rowDecorationBeforeTextGeometryCommitCounts,
                 sparseBeforeTextCommits);
        QVector<quint64> sparseAfterTextCommits =
            updatedOverlay.rowDecorationAfterTextGeometryCommitCounts;
        ++sparseAfterTextCommits[0];
        ++sparseAfterTextCommits[2];
        QCOMPARE(sparse.rowDecorationAfterTextGeometryCommitCounts,
                 sparseAfterTextCommits);
        QCOMPARE(sparse.globalBackgroundGeometryCommitCount,
                 updatedOverlay.globalBackgroundGeometryCommitCount);
        QCOMPARE(sparse.globalDecorationBeforeTextGeometryCommitCount,
                 updatedOverlay.globalDecorationBeforeTextGeometryCommitCount);
        QCOMPARE(sparse.globalDecorationAfterTextGeometryCommitCount,
                 updatedOverlay.globalDecorationAfterTextGeometryCommitCount);
    } else {
        QCOMPARE(sparse.globalBackgroundGeometryCommitCount,
                 updatedOverlay.globalBackgroundGeometryCommitCount + 1);
        QCOMPARE(sparse.globalDecorationBeforeTextGeometryCommitCount,
                 updatedOverlay.globalDecorationBeforeTextGeometryCommitCount
                     + 1);
        QCOMPARE(sparse.globalDecorationAfterTextGeometryCommitCount,
                 updatedOverlay.globalDecorationAfterTextGeometryCommitCount
                     + 1);
    }

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
    QVector<quint64> cursorSolidBuildCounts = cursorOnFirst.rowSolidBuildCounts;
    ++cursorSolidBuildCounts[0];
    ++cursorSolidBuildCounts[2];
    QCOMPARE(cursorOnThird.rowSolidBuildCounts, cursorSolidBuildCounts);
    QCOMPARE(cursorOnThird.solidCellVisitCount,
             cursorOnFirst.solidCellVisitCount
                 + static_cast<quint64>(2 * columns));
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
    QVector<quint64> searchedBuildCounts = cursorOnThird.rowBuildCounts;
    ++searchedBuildCounts[1];
    QCOMPARE(searched.rowBuildCounts, searchedBuildCounts);
    QVector<quint64> searchedSolidBuildCounts =
        cursorOnThird.rowSolidBuildCounts;
    ++searchedSolidBuildCounts[1];
    QCOMPARE(searched.rowSolidBuildCounts, searchedSolidBuildCounts);
    QCOMPARE(searched.solidCellVisitCount,
             cursorOnThird.solidCellVisitCount + static_cast<quint64>(columns));
    QVERIFY(approximatelyEqual(centerColor(searchImage, 1),
                               QColor(QStringLiteral("#abcdef"))));

    search.active = false;
    controller->searchUpdated(search);
    const QImage clearedSearchImage = window.grabWindow();
    QVERIFY(!clearedSearchImage.isNull());
    const TerminalPaneRenderProbeSnapshot clearedSearch =
        terminalPaneRenderProbe(pane);
    QVector<quint64> clearedSearchBuildCounts = searched.rowBuildCounts;
    ++clearedSearchBuildCounts[1];
    QCOMPARE(clearedSearch.rowBuildCounts, clearedSearchBuildCounts);
    QVector<quint64> clearedSearchSolidBuildCounts =
        searched.rowSolidBuildCounts;
    ++clearedSearchSolidBuildCounts[1];
    QCOMPARE(clearedSearch.rowSolidBuildCounts, clearedSearchSolidBuildCounts);
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
    for (int row = 0; row < rows; ++row) {
        QCOMPARE(paletteChanged.rowSolidBuildCounts[row],
                 clearedSearch.rowSolidBuildCounts[row] + 1);
    }

    LaunchOptions appearanceChanged = options;
    appearanceChanged.appearance.faintOpacity = 0.75;
    pane->applyRuntimeOptions(appearanceChanged);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot reloadedAppearance =
        terminalPaneRenderProbe(pane);
    QCOMPARE(reloadedAppearance.rootSerial, paletteChanged.rootSerial);
    QVERIFY(allVisibleRowsRebuilt(paletteChanged, reloadedAppearance));
    for (int row = 0; row < rows; ++row) {
        QCOMPARE(reloadedAppearance.rowSolidBuildCounts[row],
                 paletteChanged.rowSolidBuildCounts[row] + 1);
    }

    LaunchOptions styleChanged = appearanceChanged;
    styleChanged.typography.face(TerminalFontRole::Bold).style =
        TerminalFontStyles::Disabled{};
    pane->applyRuntimeOptions(styleChanged);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot reloadedStyle =
        terminalPaneRenderProbe(pane);
    QCOMPARE(reloadedStyle.rootSerial, reloadedAppearance.rootSerial);
    QVERIFY(allVisibleRowsRebuilt(reloadedAppearance, reloadedStyle));
    QCOMPARE(reloadedStyle.rowSolidBuildCounts,
             reloadedAppearance.rowSolidBuildCounts);

    LaunchOptions fontChanged = styleChanged;
    fontChanged.typography.pointSize += 1.0;
    pane->applyRuntimeOptions(fontChanged);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot reloadedFont =
        terminalPaneRenderProbe(pane);
    QCOMPARE(reloadedFont.rootSerial, reloadedStyle.rootSerial);
    QVERIFY(allVisibleRowsRebuilt(reloadedStyle, reloadedFont));

    pane->setWidth(std::max(1.0, pane->width() - cellWidth));
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot resized =
        terminalPaneRenderProbe(pane);
    QCOMPARE(resized.rootSerial, reloadedFont.rootSerial);
    QVERIFY(allVisibleRowsRebuilt(reloadedFont, resized));

    window.close();
    delete pane;
}

void TerminalPaneTest::invalidatesOnlyChangedSearchDecorationRows()
{
    qRegisterMetaType<TerminalSearchUpdate>();
    qRegisterMetaType<TerminalUpdate>();

    const QColor baseForeground(QStringLiteral("#dddddd"));
    const QColor baseBackground(QStringLiteral("#101010"));
    const QColor candidateForeground(QStringLiteral("#abcdef"));
    const QColor candidateBackground(QStringLiteral("#123456"));
    const QColor selectedForeground(QStringLiteral("#fedcba"));
    const QColor selectedBackground(QStringLiteral("#654321"));

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    useSystemFixedFont(options);
    options.appearance.foregroundColor = baseForeground;
    options.appearance.backgroundColor = baseBackground;
    options.appearance.searchForeground =
        TerminalColorValue::fromColor(candidateForeground);
    options.appearance.searchBackground =
        TerminalColorValue::fromColor(candidateBackground);
    options.appearance.searchSelectedForeground =
        TerminalColorValue::fromColor(selectedForeground);
    options.appearance.searchSelectedBackground =
        TerminalColorValue::fromColor(selectedBackground);

    const TerminalCellMetrics metrics = terminalCellMetrics(options.typography);
    constexpr int columns = 4;
    constexpr int rows = 4;
    constexpr qsizetype cellCount = columns * rows;
    QQuickWindow window;
    window.setColor(baseBackground);
    window.resize(qCeil(metrics.cellWidth * columns),
                  qCeil(metrics.cellHeight * rows));
    auto *pane = new TerminalPane(options, window.contentItem(), std::nullopt,
                                  TerminalSessionStartMode::Deferred);
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);

    TerminalUpdate frame;
    frame.columns = columns;
    frame.rows = rows;
    frame.fullFrame = true;
    frame.colorsChanged = true;
    frame.foreground = baseForeground;
    frame.background = baseBackground;
    frame.cursorChanged = true;
    frame.cursorVisible = false;
    frame.contentRevision = 73;
    for (int row = 0; row < rows; ++row) {
        TerminalRowUpdate rowUpdate;
        rowUpdate.row = row;
        rowUpdate.cells.resize(columns);
        for (TerminalCell &cell : rowUpdate.cells) {
            cell.text = QStringLiteral("x");
            cell.foreground = baseForeground;
            cell.background = baseBackground;
            cell.backgroundExplicit = true;
        }
        frame.dirtyRows.append(std::move(rowUpdate));
    }
    TerminalCell &wideHead = frame.dirtyRows[2].cells[1];
    wideHead.text = QString(QChar(0x754c));
    wideHead.columnSpan = 2;
    TerminalCell &wideSpacer = frame.dirtyRows[2].cells[2];
    wideSpacer.text.clear();
    wideSpacer.spacer = true;
    controller->terminalUpdated(frame);

    const auto paint = [&] {
        pane->update();
        const QImage image = window.grabWindow();
        if (image.isNull()) return TerminalPaneRenderProbeSnapshot{};
        return terminalPaneRenderProbe(pane);
    };
    QTRY_COMPARE_WITH_TIMEOUT(paint().rowBuildCounts.size(), rows, 3000);
    const TerminalPaneRenderProbeSnapshot initial = paint();
    QCOMPARE(initial.rowSolidBuildCounts.size(), rows);
    QCOMPARE(initial.cellBackgrounds.size(), cellCount);
    QCOMPARE(initial.glyphForegrounds.size(), cellCount);

    const auto incrementedRows = [](QVector<quint64> counts,
                                    std::initializer_list<int> changed) {
        for (const int row : changed) ++counts[row];
        return counts;
    };
    const auto cellIndex = [](int column, int row) {
        return static_cast<qsizetype>(row) * columns + column;
    };
    const auto verifyRetainedRoot =
        [](const TerminalPaneRenderProbeSnapshot &before,
           const TerminalPaneRenderProbeSnapshot &after) {
            QCOMPARE(after.rootSerial, before.rootSerial);
            QCOMPARE(after.rowNodeSerials, before.rowNodeSerials);
        };

    TerminalSearchUpdate search;
    search.generation = 1;
    search.contentRevision = frame.contentRevision;
    search.active = true;
    search.complete = false;
    search.totalMatches = 1;
    search.selectedMatch = -1;
    search.columns = columns;
    search.rows = rows;
    search.visibleCellMask = cellMask(columns, rows, {QPoint(0, 1)});
    search.selectedCellMask = QBitArray(cellCount);
    controller->searchUpdated(search);
    const TerminalPaneRenderProbeSnapshot candidate = paint();
    verifyRetainedRoot(initial, candidate);
    QCOMPARE(candidate.rowBuildCounts,
             incrementedRows(initial.rowBuildCounts, {1}));
    QCOMPARE(candidate.rowSolidBuildCounts,
             incrementedRows(initial.rowSolidBuildCounts, {1}));
    QCOMPARE(candidate.solidCellVisitCount,
             initial.solidCellVisitCount + static_cast<quint64>(columns));
    QCOMPARE(candidate.cellBackgrounds.at(cellIndex(0, 1)),
             candidateBackground);
    QCOMPARE(candidate.glyphForegrounds.at(cellIndex(0, 1)),
             candidateForeground);
    QCOMPARE(candidate.cellBackgrounds.at(cellIndex(0, 0)), baseBackground);
    QCOMPARE(candidate.glyphForegrounds.at(cellIndex(0, 0)), baseForeground);

    search.complete = true;
    search.selectedMatch = 0;
    search.selectedCellMask = cellMask(columns, rows, {QPoint(0, 1)});
    controller->searchUpdated(search);
    const TerminalPaneRenderProbeSnapshot selected = paint();
    verifyRetainedRoot(candidate, selected);
    QCOMPARE(selected.rowBuildCounts,
             incrementedRows(candidate.rowBuildCounts, {1}));
    QCOMPARE(selected.rowSolidBuildCounts,
             incrementedRows(candidate.rowSolidBuildCounts, {1}));
    QCOMPARE(selected.solidCellVisitCount,
             candidate.solidCellVisitCount + static_cast<quint64>(columns));
    QCOMPARE(selected.cellBackgrounds.at(cellIndex(0, 1)), selectedBackground);
    QCOMPARE(selected.glyphForegrounds.at(cellIndex(0, 1)), selectedForeground);

    // Moving the candidate clears its old row and decorates its new row. A
    // mask bit on a wide-cell head must also decorate the spacer cell.
    search.complete = false;
    search.selectedMatch = -1;
    search.visibleCellMask = cellMask(columns, rows, {QPoint(1, 2)});
    search.selectedCellMask = QBitArray(cellCount);
    controller->searchUpdated(search);
    const TerminalPaneRenderProbeSnapshot wideCandidate = paint();
    verifyRetainedRoot(selected, wideCandidate);
    QCOMPARE(wideCandidate.rowBuildCounts,
             incrementedRows(selected.rowBuildCounts, {1, 2}));
    QCOMPARE(wideCandidate.rowSolidBuildCounts,
             incrementedRows(selected.rowSolidBuildCounts, {1, 2}));
    QCOMPARE(wideCandidate.solidCellVisitCount,
             selected.solidCellVisitCount + static_cast<quint64>(2 * columns));
    QCOMPARE(wideCandidate.cellBackgrounds.at(cellIndex(0, 1)), baseBackground);
    QCOMPARE(wideCandidate.glyphForegrounds.at(cellIndex(0, 1)),
             baseForeground);
    for (const int column : {1, 2}) {
        QCOMPARE(wideCandidate.cellBackgrounds.at(cellIndex(column, 2)),
                 candidateBackground);
        QCOMPARE(wideCandidate.glyphForegrounds.at(cellIndex(column, 2)),
                 candidateForeground);
    }

    search.complete = true;
    search.selectedMatch = 0;
    search.selectedCellMask = cellMask(columns, rows, {QPoint(1, 2)});
    controller->searchUpdated(search);
    const TerminalPaneRenderProbeSnapshot wideSelected = paint();
    verifyRetainedRoot(wideCandidate, wideSelected);
    QCOMPARE(wideSelected.rowBuildCounts,
             incrementedRows(wideCandidate.rowBuildCounts, {2}));
    QCOMPARE(wideSelected.rowSolidBuildCounts,
             incrementedRows(wideCandidate.rowSolidBuildCounts, {2}));
    QCOMPARE(wideSelected.solidCellVisitCount,
             wideCandidate.solidCellVisitCount + static_cast<quint64>(columns));
    for (const int column : {1, 2}) {
        QCOMPARE(wideSelected.cellBackgrounds.at(cellIndex(column, 2)),
                 selectedBackground);
        QCOMPARE(wideSelected.glyphForegrounds.at(cellIndex(column, 2)),
                 selectedForeground);
    }

    search.active = false;
    controller->searchUpdated(search);
    const TerminalPaneRenderProbeSnapshot cleared = paint();
    verifyRetainedRoot(wideSelected, cleared);
    QCOMPARE(cleared.rowBuildCounts,
             incrementedRows(wideSelected.rowBuildCounts, {2}));
    QCOMPARE(cleared.rowSolidBuildCounts,
             incrementedRows(wideSelected.rowSolidBuildCounts, {2}));
    QCOMPARE(cleared.solidCellVisitCount,
             wideSelected.solidCellVisitCount + static_cast<quint64>(columns));
    for (const int column : {1, 2}) {
        QCOMPARE(cleared.cellBackgrounds.at(cellIndex(column, 2)),
                 baseBackground);
        QCOMPARE(cleared.glyphForegrounds.at(cellIndex(column, 2)),
                 baseForeground);
    }

    search.active = true;
    search.complete = false;
    search.selectedMatch = -1;
    search.visibleCellMask = cellMask(columns, rows, {QPoint(3, 3)});
    search.selectedCellMask = QBitArray(cellCount);
    controller->searchUpdated(search);
    const TerminalPaneRenderProbeSnapshot beforeMalformed = paint();
    verifyRetainedRoot(cleared, beforeMalformed);
    QCOMPARE(beforeMalformed.rowBuildCounts,
             incrementedRows(cleared.rowBuildCounts, {3}));
    QCOMPARE(beforeMalformed.rowSolidBuildCounts,
             incrementedRows(cleared.rowSolidBuildCounts, {3}));
    QCOMPARE(beforeMalformed.cellBackgrounds.at(cellIndex(3, 3)),
             candidateBackground);

    // A malformed pair is rejected atomically. Replacing a valid mask with
    // it clears only the previously decorated row.
    search.visibleCellMask = QBitArray(cellCount - 1);
    search.visibleCellMask.setBit(cellCount - 2);
    search.selectedCellMask = QBitArray(cellCount - 1);
    controller->searchUpdated(search);
    const TerminalPaneRenderProbeSnapshot malformed = paint();
    verifyRetainedRoot(beforeMalformed, malformed);
    QCOMPARE(malformed.rowBuildCounts,
             incrementedRows(beforeMalformed.rowBuildCounts, {3}));
    QCOMPARE(malformed.rowSolidBuildCounts,
             incrementedRows(beforeMalformed.rowSolidBuildCounts, {3}));
    QCOMPARE(malformed.solidCellVisitCount,
             beforeMalformed.solidCellVisitCount
                 + static_cast<quint64>(columns));
    QCOMPARE(malformed.cellBackgrounds.at(cellIndex(3, 3)), baseBackground);
    QCOMPARE(malformed.glyphForegrounds.at(cellIndex(3, 3)), baseForeground);

    // A valid candidate mask paired with a malformed selected mask is also
    // rejected, and empty-to-empty rejection must retain every row.
    search.visibleCellMask = cellMask(columns, rows, {QPoint(0, 0)});
    search.selectedCellMask = QBitArray(cellCount - 1);
    controller->searchUpdated(search);
    const TerminalPaneRenderProbeSnapshot malformedPair = paint();
    verifyRetainedRoot(malformed, malformedPair);
    QCOMPARE(malformedPair.rowBuildCounts, malformed.rowBuildCounts);
    QCOMPARE(malformedPair.rowSolidBuildCounts, malformed.rowSolidBuildCounts);
    QCOMPARE(malformedPair.solidCellVisitCount, malformed.solidCellVisitCount);
    QCOMPARE(malformedPair.cellBackgrounds.at(cellIndex(0, 0)), baseBackground);
    QCOMPARE(malformedPair.glyphForegrounds.at(cellIndex(0, 0)),
             baseForeground);

    window.close();
    delete pane;
}

void TerminalPaneTest::rendersAndRetainsKittyGraphics()
{
    qRegisterMetaType<TerminalUpdate>();

    const bool expectRhi =
        qEnvironmentVariableIntValue("GHOSTTY_QT_EXPECT_RHI") != 0;
    if (expectRhi) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    }

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    useSystemFixedFont(options);
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(320, 180);
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy sessionEnded(pane, &TerminalPane::sessionEnded);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(sessionEnded.count(), 1, 5000);

    const TerminalPaneRenderProbeSnapshot before =
        terminalPaneRenderProbe(pane);
    const auto layout = terminalViewportLayout({
        .surfaceSize = pane->size(),
        .cellSize = QSizeF(before.metrics.cellWidth, before.metrics.cellHeight),
        .devicePixelRatio = window.devicePixelRatio(),
        .padding = options.padding,
    });
    QVERIFY(layout.has_value());
    QVERIFY(layout->session.columns >= 2);
    QVERIFY(layout->session.rows >= 3);

    const auto makeSnapshot =
        [&layout](
            const std::shared_ptr<const TerminalKittyGraphicsImage> &asset,
            int columnOffset = 0) {
            auto snapshot = std::make_shared<TerminalKittyGraphicsSnapshot>();
            snapshot->storageGeneration =
                asset != nullptr ? asset->generation : quint64{99};
            snapshot->cellWidthPixels =
                static_cast<quint32>(layout->session.cellWidthPixels);
            snapshot->cellHeightPixels =
                static_cast<quint32>(layout->session.cellHeightPixels);
            if (asset != nullptr) {
                const std::array layers{
                    std::pair{qint32{-1'073'741'825},
                              TerminalKittyGraphicsLayer::BelowBackground},
                    std::pair{qint32{-1},
                              TerminalKittyGraphicsLayer::BelowText},
                    std::pair{qint32{1}, TerminalKittyGraphicsLayer::AboveText},
                };
                for (int row = 0; row < 3; ++row) {
                    snapshot->placements.append({
                        .image = asset,
                        .placementId = static_cast<quint32>(row + 1),
                        .z = layers.at(static_cast<size_t>(row)).first,
                        .layer = layers.at(static_cast<size_t>(row)).second,
                        .viewportColumn = columnOffset,
                        .viewportRow = row,
                        .destinationWidthPixels = static_cast<quint32>(
                            layout->session.cellWidthPixels),
                        .destinationHeightPixels = static_cast<quint32>(
                            layout->session.cellHeightPixels),
                        .sourceWidth = 2,
                        .sourceHeight = 2,
                    });
                }
            }
            return snapshot;
        };
    const auto fullUpdate = [&layout](const auto &snapshot, quint64 revision) {
        TerminalUpdate update;
        update.columns = layout->session.columns;
        update.rows = layout->session.rows;
        update.fullFrame = true;
        update.colorsChanged = true;
        update.foreground = Qt::white;
        update.background = Qt::black;
        update.cursorColor = Qt::white;
        update.kittyGraphicsChanged = true;
        update.kittyGraphics = snapshot;
        update.contentRevision = revision;
        for (int row = 0; row < update.rows; ++row) {
            TerminalRowUpdate rowUpdate;
            rowUpdate.row = row;
            rowUpdate.cells.resize(update.columns);
            for (TerminalCell &cell : rowUpdate.cells) {
                cell.foreground = Qt::white;
                cell.background = Qt::black;
            }
            update.dirtyRows.append(std::move(rowUpdate));
        }
        return update;
    };
    const auto graphicsUpdate = [&layout](const auto &snapshot,
                                          quint64 revision) {
        TerminalUpdate update;
        update.columns = layout->session.columns;
        update.rows = layout->session.rows;
        update.kittyGraphicsChanged = true;
        update.kittyGraphics = snapshot;
        update.contentRevision = revision;
        return update;
    };
    const auto paintedProbe = [&window, pane] {
        const QImage image = window.grabWindow();
        if (image.isNull()) return TerminalPaneRenderProbeSnapshot{};
        return terminalPaneRenderProbe(pane);
    };

    const auto firstAsset = verticalKittyImage(10);
    controller->terminalUpdated(fullUpdate(makeSnapshot(firstAsset), 1));
    const QImage initialImage = window.grabWindow();
    QVERIFY(!initialImage.isNull());
    if (expectRhi) {
        const auto graphicsApi = window.rendererInterface()->graphicsApi();
        QCOMPARE(graphicsApi, QSGRendererInterface::OpenGL);
        QVERIFY(QSGRendererInterface::isApiRhiBased(graphicsApi));
    }
    const qreal renderedCellWidth =
        layout->gridRect.width() / layout->session.columns;
    const qreal renderedCellHeight =
        layout->gridRect.height() / layout->session.rows;
    for (int row = 0; row < 3; ++row) {
        const QColor topPixel =
            itemPixel(window, *pane, initialImage,
                      QPointF(layout->gridRect.left() + renderedCellWidth / 2.0,
                              layout->gridRect.top()
                                  + (static_cast<qreal>(row) + 0.25)
                                      * renderedCellHeight));
        QVERIFY2(approximatelyEqual(topPixel, QColor(128, 0, 0)),
                 qPrintable(topPixel.name(QColor::HexArgb)));
        const QColor bottomPixel =
            itemPixel(window, *pane, initialImage,
                      QPointF(layout->gridRect.left() + renderedCellWidth / 2.0,
                              layout->gridRect.top()
                                  + (static_cast<qreal>(row) + 0.75)
                                      * renderedCellHeight));
        QVERIFY2(approximatelyEqual(bottomPixel, QColor(0, 0, 255)),
                 qPrintable(bottomPixel.name(QColor::HexArgb)));
        if (expectRhi) {
            const QPointF boundaryPosition(
                layout->gridRect.left() + renderedCellWidth / 2.0,
                layout->gridRect.top()
                    + (static_cast<qreal>(row) + 0.5) * renderedCellHeight);
            const QColor boundaryPixel =
                itemPixel(window, *pane, initialImage, boundaryPosition);
            const QPointF boundaryScene = pane->mapToScene(boundaryPosition);
            const qreal yScale =
                static_cast<qreal>(initialImage.height()) / window.height();
            const int sampledY = std::clamp(
                static_cast<int>(std::floor(boundaryScene.y() * yScale)), 0,
                initialImage.height() - 1);
            const qreal sampledItemY =
                pane->mapFromScene(
                        QPointF(boundaryScene.x(),
                                (static_cast<qreal>(sampledY) + 0.5) / yScale))
                    .y();
            const qreal destinationTop = layout->gridRect.top()
                + static_cast<qreal>(row) * renderedCellHeight;
            const qreal blueWeight = std::clamp(
                2.0 * (sampledItemY - destinationTop) / renderedCellHeight
                    - 0.5,
                0.0, 1.0);
            QVERIFY2(blueWeight > 0.1 && blueWeight < 0.9,
                     qPrintable(QString::number(blueWeight)));
            const qreal interpolatedAlpha =
                (128.0 * (1.0 - blueWeight) + 255.0 * blueWeight) / 255.0;
            const QColor expectedBoundary(
                qRound(255.0 * (1.0 - blueWeight) * interpolatedAlpha), 0,
                qRound(255.0 * blueWeight * interpolatedAlpha));
            const QColor premultipliedBeforeFiltering(
                qRound(128.0 * (1.0 - blueWeight)), 0,
                qRound(255.0 * blueWeight));
            // Straight RGBA must be interpolated first and only then
            // premultiplied. Upload-time premultiplication would skew this
            // boundary toward opaque blue instead.
            QVERIFY(!approximatelyEqual(expectedBoundary,
                                        premultipliedBeforeFiltering));
            QVERIFY2(approximatelyEqual(boundaryPixel, expectedBoundary),
                     qPrintable(boundaryPixel.name(QColor::HexArgb)));
        }
    }
    const TerminalPaneRenderProbeSnapshot initial =
        terminalPaneRenderProbe(pane);
    QCOMPARE(initial.kittyGraphicsDestinations.size(), 3);
    QCOMPARE(initial.kittyGraphicsSources,
             QVector<QRectF>(
                 {QRectF(0, 0, 2, 2), QRectF(0, 0, 2, 2), QRectF(0, 0, 2, 2)}));
    QCOMPARE(initial.kittyGraphicsLayers,
             QVector<TerminalKittyGraphicsLayer>(
                 {TerminalKittyGraphicsLayer::BelowBackground,
                  TerminalKittyGraphicsLayer::BelowText,
                  TerminalKittyGraphicsLayer::AboveText}));
    QCOMPARE(initial.kittyGraphicsTextureCount, qsizetype{1});
    QCOMPARE(initial.kittyGraphicsTextureBytes, quint64{16});
    const quint64 uploadsPerAsset = initial.kittyGraphicsTextureUploadCount;
    QCOMPARE(uploadsPerAsset, quint64{1});
    QCOMPARE(initial.kittyGraphicsNodeCreationCount,
             before.kittyGraphicsNodeCreationCount + 3);
    QCOMPARE(initial.kittyGraphicsNodeDeletionCount,
             before.kittyGraphicsNodeDeletionCount);
    QCOMPARE(initial.kittyGraphicsGeometryWriteCount,
             before.kittyGraphicsGeometryWriteCount + 3);
    QCOMPARE(initial.kittyGraphicsMaterialAssignmentCount,
             before.kittyGraphicsMaterialAssignmentCount + 3);
    QCOMPARE(initial.kittyGraphicsTextureSetEvictionCount,
             before.kittyGraphicsTextureSetEvictionCount);

    TerminalUpdate metadata;
    metadata.columns = layout->session.columns;
    metadata.rows = layout->session.rows;
    metadata.scrollbarChanged = true;
    metadata.scrollTotal = 10;
    metadata.scrollLength = 5;
    metadata.contentRevision = 2;
    controller->terminalUpdated(metadata);
    const TerminalPaneRenderProbeSnapshot retained = paintedProbe();
    QCOMPARE(retained.kittyGraphicsTextureUploadCount, uploadsPerAsset);
    QCOMPARE(retained.kittyGraphicsDestinations,
             initial.kittyGraphicsDestinations);
    QCOMPARE(retained.kittyGraphicsNodeCreationCount,
             initial.kittyGraphicsNodeCreationCount);
    QCOMPARE(retained.kittyGraphicsNodeDeletionCount,
             initial.kittyGraphicsNodeDeletionCount);
    QCOMPARE(retained.kittyGraphicsGeometryWriteCount,
             initial.kittyGraphicsGeometryWriteCount);
    QCOMPARE(retained.kittyGraphicsMaterialAssignmentCount,
             initial.kittyGraphicsMaterialAssignmentCount);
    QCOMPARE(retained.kittyGraphicsTextureSetEvictionCount,
             initial.kittyGraphicsTextureSetEvictionCount);

    controller->terminalUpdated(graphicsUpdate(makeSnapshot(firstAsset), 3));
    const TerminalPaneRenderProbeSnapshot equivalent = paintedProbe();
    QCOMPARE(equivalent.kittyGraphicsDestinations,
             initial.kittyGraphicsDestinations);
    QCOMPARE(equivalent.kittyGraphicsNodeCreationCount,
             retained.kittyGraphicsNodeCreationCount);
    QCOMPARE(equivalent.kittyGraphicsNodeDeletionCount,
             retained.kittyGraphicsNodeDeletionCount);
    QCOMPARE(equivalent.kittyGraphicsGeometryWriteCount,
             retained.kittyGraphicsGeometryWriteCount);
    QCOMPARE(equivalent.kittyGraphicsMaterialAssignmentCount,
             retained.kittyGraphicsMaterialAssignmentCount);
    QCOMPARE(equivalent.kittyGraphicsTextureSetEvictionCount,
             retained.kittyGraphicsTextureSetEvictionCount);

    controller->terminalUpdated(graphicsUpdate(makeSnapshot(firstAsset, 1), 4));
    const TerminalPaneRenderProbeSnapshot moved = paintedProbe();
    QCOMPARE(moved.kittyGraphicsTextureUploadCount, uploadsPerAsset);
    QCOMPARE(moved.kittyGraphicsTextureCount, qsizetype{1});
    QCOMPARE(moved.kittyGraphicsTextureBytes, quint64{16});
    QCOMPARE(moved.kittyGraphicsDestinations.constFirst().x(),
             renderedCellWidth);
    QCOMPARE(moved.kittyGraphicsNodeCreationCount,
             equivalent.kittyGraphicsNodeCreationCount);
    QCOMPARE(moved.kittyGraphicsNodeDeletionCount,
             equivalent.kittyGraphicsNodeDeletionCount);
    QCOMPARE(moved.kittyGraphicsGeometryWriteCount,
             equivalent.kittyGraphicsGeometryWriteCount + 3);
    QCOMPARE(moved.kittyGraphicsMaterialAssignmentCount,
             equivalent.kittyGraphicsMaterialAssignmentCount);
    QCOMPARE(moved.kittyGraphicsTextureSetEvictionCount,
             equivalent.kittyGraphicsTextureSetEvictionCount);

    const auto replacement = kittyImage(11, Qt::green);
    controller->terminalUpdated(
        graphicsUpdate(makeSnapshot(replacement, 1), 5));
    const TerminalPaneRenderProbeSnapshot replaced = paintedProbe();
    QCOMPARE(replaced.kittyGraphicsTextureUploadCount, uploadsPerAsset * 2);
    QCOMPARE(replaced.kittyGraphicsTextureCount, qsizetype{1});
    QCOMPARE(replaced.kittyGraphicsTextureBytes, quint64{16});
    QCOMPARE(replaced.kittyGraphicsNodeCreationCount,
             moved.kittyGraphicsNodeCreationCount);
    QCOMPARE(replaced.kittyGraphicsNodeDeletionCount,
             moved.kittyGraphicsNodeDeletionCount);
    QCOMPARE(replaced.kittyGraphicsGeometryWriteCount,
             moved.kittyGraphicsGeometryWriteCount);
    QCOMPARE(replaced.kittyGraphicsMaterialAssignmentCount,
             moved.kittyGraphicsMaterialAssignmentCount + 3);
    QCOMPARE(replaced.kittyGraphicsTextureSetEvictionCount,
             moved.kittyGraphicsTextureSetEvictionCount + 1);

    const auto secondReplacement = kittyImage(12, Qt::yellow);
    controller->terminalUpdated(
        graphicsUpdate(makeSnapshot(secondReplacement, 1), 6));
    const QImage replacedAgainImage = window.grabWindow();
    QVERIFY(!replacedAgainImage.isNull());
    const TerminalPaneRenderProbeSnapshot replacedAgain =
        terminalPaneRenderProbe(pane);
    QCOMPARE(replacedAgain.kittyGraphicsTextureUploadCount,
             replaced.kittyGraphicsTextureUploadCount + 1);
    QCOMPARE(replacedAgain.kittyGraphicsTextureCount, qsizetype{1});
    QCOMPARE(replacedAgain.kittyGraphicsNodeCreationCount,
             replaced.kittyGraphicsNodeCreationCount);
    QCOMPARE(replacedAgain.kittyGraphicsNodeDeletionCount,
             replaced.kittyGraphicsNodeDeletionCount);
    QCOMPARE(replacedAgain.kittyGraphicsGeometryWriteCount,
             replaced.kittyGraphicsGeometryWriteCount);
    QCOMPARE(replacedAgain.kittyGraphicsMaterialAssignmentCount,
             replaced.kittyGraphicsMaterialAssignmentCount + 3);
    QCOMPARE(replacedAgain.kittyGraphicsTextureSetEvictionCount,
             replaced.kittyGraphicsTextureSetEvictionCount + 1);
    const QColor opaquePixel =
        itemPixel(window, *pane, replacedAgainImage,
                  QPointF(layout->gridRect.left() + 1.5 * renderedCellWidth,
                          layout->gridRect.top() + 2.5 * renderedCellHeight));
    QVERIFY2(approximatelyEqual(opaquePixel, Qt::yellow),
             qPrintable(opaquePixel.name(QColor::HexArgb)));

    auto staleGeometry = makeSnapshot(secondReplacement, 1);
    ++staleGeometry->cellWidthPixels;
    controller->terminalUpdated(graphicsUpdate(staleGeometry, 7));
    const TerminalPaneRenderProbeSnapshot hidden = paintedProbe();
    QVERIFY(hidden.kittyGraphicsDestinations.isEmpty());
    QCOMPARE(hidden.kittyGraphicsTextureCount, qsizetype{0});
    QCOMPARE(hidden.kittyGraphicsTextureBytes, quint64{0});
    QCOMPARE(hidden.kittyGraphicsNodeCreationCount,
             replacedAgain.kittyGraphicsNodeCreationCount);
    QCOMPARE(hidden.kittyGraphicsNodeDeletionCount,
             replacedAgain.kittyGraphicsNodeDeletionCount + 3);
    QCOMPARE(hidden.kittyGraphicsGeometryWriteCount,
             replacedAgain.kittyGraphicsGeometryWriteCount);
    QCOMPARE(hidden.kittyGraphicsMaterialAssignmentCount,
             replacedAgain.kittyGraphicsMaterialAssignmentCount);
    QCOMPARE(hidden.kittyGraphicsTextureSetEvictionCount,
             replacedAgain.kittyGraphicsTextureSetEvictionCount + 1);

    controller->terminalUpdated(
        graphicsUpdate(makeSnapshot(secondReplacement, 1), 8));
    const TerminalPaneRenderProbeSnapshot restored = paintedProbe();
    QCOMPARE(restored.kittyGraphicsDestinations.size(), 3);
    QCOMPARE(restored.kittyGraphicsTextureUploadCount,
             hidden.kittyGraphicsTextureUploadCount + 1);
    QCOMPARE(restored.kittyGraphicsTextureCount, qsizetype{1});
    QCOMPARE(restored.kittyGraphicsNodeCreationCount,
             hidden.kittyGraphicsNodeCreationCount + 3);
    QCOMPARE(restored.kittyGraphicsNodeDeletionCount,
             hidden.kittyGraphicsNodeDeletionCount);
    QCOMPARE(restored.kittyGraphicsGeometryWriteCount,
             hidden.kittyGraphicsGeometryWriteCount + 3);
    QCOMPARE(restored.kittyGraphicsMaterialAssignmentCount,
             hidden.kittyGraphicsMaterialAssignmentCount + 3);
    QCOMPARE(restored.kittyGraphicsTextureSetEvictionCount,
             hidden.kittyGraphicsTextureSetEvictionCount);

    controller->terminalUpdated(graphicsUpdate(makeSnapshot(nullptr), 9));
    const TerminalPaneRenderProbeSnapshot deleted = paintedProbe();
    QVERIFY(deleted.kittyGraphicsDestinations.isEmpty());
    QCOMPARE(deleted.kittyGraphicsTextureCount, qsizetype{0});
    QCOMPARE(deleted.kittyGraphicsNodeCreationCount,
             restored.kittyGraphicsNodeCreationCount);
    QCOMPARE(deleted.kittyGraphicsNodeDeletionCount,
             restored.kittyGraphicsNodeDeletionCount + 3);
    QCOMPARE(deleted.kittyGraphicsGeometryWriteCount,
             restored.kittyGraphicsGeometryWriteCount);
    QCOMPARE(deleted.kittyGraphicsMaterialAssignmentCount,
             restored.kittyGraphicsMaterialAssignmentCount);
    QCOMPARE(deleted.kittyGraphicsTextureSetEvictionCount,
             restored.kittyGraphicsTextureSetEvictionCount + 1);

    const auto makeOverlappingSnapshot =
        [&layout](
            const std::shared_ptr<const TerminalKittyGraphicsImage> &first,
            const std::shared_ptr<const TerminalKittyGraphicsImage> &second) {
            auto snapshot = std::make_shared<TerminalKittyGraphicsSnapshot>();
            snapshot->storageGeneration =
                std::max(first->generation, second->generation);
            snapshot->cellWidthPixels =
                static_cast<quint32>(layout->session.cellWidthPixels);
            snapshot->cellHeightPixels =
                static_cast<quint32>(layout->session.cellHeightPixels);
            for (const auto &asset : std::array{first, second}) {
                snapshot->placements.append({
                    .image = asset,
                    .placementId = 0,
                    .z = 1,
                    .layer = TerminalKittyGraphicsLayer::AboveText,
                    .viewportColumn = 1,
                    .viewportRow = 0,
                    .destinationWidthPixels =
                        static_cast<quint32>(layout->session.cellWidthPixels),
                    .destinationHeightPixels =
                        static_cast<quint32>(layout->session.cellHeightPixels),
                    .sourceWidth = 2,
                    .sourceHeight = 2,
                });
            }
            return snapshot;
        };
    const auto red = kittyImage(20, Qt::red);
    const auto blue = kittyImage(21, Qt::blue);
    controller->terminalUpdated(
        graphicsUpdate(makeOverlappingSnapshot(red, blue), 10));
    const QImage overlappingImage = window.grabWindow();
    QVERIFY(!overlappingImage.isNull());
    const TerminalPaneRenderProbeSnapshot overlapping =
        terminalPaneRenderProbe(pane);
    QCOMPARE(overlapping.kittyGraphicsTextureUploadCount,
             deleted.kittyGraphicsTextureUploadCount + 2);
    QCOMPARE(overlapping.kittyGraphicsTextureCount, qsizetype{2});
    QCOMPARE(overlapping.kittyGraphicsNodeCreationCount,
             deleted.kittyGraphicsNodeCreationCount + 2);
    QCOMPARE(overlapping.kittyGraphicsNodeDeletionCount,
             deleted.kittyGraphicsNodeDeletionCount);
    QCOMPARE(overlapping.kittyGraphicsGeometryWriteCount,
             deleted.kittyGraphicsGeometryWriteCount + 2);
    QCOMPARE(overlapping.kittyGraphicsMaterialAssignmentCount,
             deleted.kittyGraphicsMaterialAssignmentCount + 2);
    QCOMPARE(overlapping.kittyGraphicsTextureSetEvictionCount,
             deleted.kittyGraphicsTextureSetEvictionCount);
    const QPointF overlapPoint(
        layout->gridRect.left() + 1.5 * renderedCellWidth,
        layout->gridRect.top() + 0.5 * renderedCellHeight);
    const QColor bluePixel =
        itemPixel(window, *pane, overlappingImage, overlapPoint);
    QVERIFY2(approximatelyEqual(bluePixel, Qt::blue),
             qPrintable(bluePixel.name(QColor::HexArgb)));

    controller->terminalUpdated(
        graphicsUpdate(makeOverlappingSnapshot(blue, red), 11));
    const QImage reorderedImage = window.grabWindow();
    QVERIFY(!reorderedImage.isNull());
    const TerminalPaneRenderProbeSnapshot reordered =
        terminalPaneRenderProbe(pane);
    QCOMPARE(reordered.kittyGraphicsTextureUploadCount,
             overlapping.kittyGraphicsTextureUploadCount);
    QCOMPARE(reordered.kittyGraphicsTextureCount, qsizetype{2});
    QCOMPARE(reordered.kittyGraphicsNodeCreationCount,
             overlapping.kittyGraphicsNodeCreationCount);
    QCOMPARE(reordered.kittyGraphicsNodeDeletionCount,
             overlapping.kittyGraphicsNodeDeletionCount);
    QCOMPARE(reordered.kittyGraphicsGeometryWriteCount,
             overlapping.kittyGraphicsGeometryWriteCount);
    QCOMPARE(reordered.kittyGraphicsMaterialAssignmentCount,
             overlapping.kittyGraphicsMaterialAssignmentCount);
    QCOMPARE(reordered.kittyGraphicsTextureSetEvictionCount,
             overlapping.kittyGraphicsTextureSetEvictionCount);
    const QColor redPixel =
        itemPixel(window, *pane, reorderedImage, overlapPoint);
    QVERIFY2(approximatelyEqual(redPixel, Qt::red),
             qPrintable(redPixel.name(QColor::HexArgb)));

    controller->terminalUpdated(graphicsUpdate(makeSnapshot(nullptr), 12));
    const TerminalPaneRenderProbeSnapshot overlapDeleted = paintedProbe();
    QCOMPARE(overlapDeleted.kittyGraphicsNodeDeletionCount,
             reordered.kittyGraphicsNodeDeletionCount + 2);
    QCOMPARE(overlapDeleted.kittyGraphicsTextureSetEvictionCount,
             reordered.kittyGraphicsTextureSetEvictionCount + 2);

    const auto makeImplicitSnapshot =
        [&layout](
            const std::shared_ptr<const TerminalKittyGraphicsImage> &asset,
            const QVector<int> &columns, TerminalKittyGraphicsLayer layer) {
            auto snapshot = std::make_shared<TerminalKittyGraphicsSnapshot>();
            snapshot->storageGeneration = asset->generation;
            snapshot->cellWidthPixels =
                static_cast<quint32>(layout->session.cellWidthPixels);
            snapshot->cellHeightPixels =
                static_cast<quint32>(layout->session.cellHeightPixels);
            for (const int column : columns) {
                snapshot->placements.append({
                    .image = asset,
                    .placementId = 0,
                    .z =
                        layer == TerminalKittyGraphicsLayer::AboveText ? 1 : -1,
                    .layer = layer,
                    .viewportColumn = column,
                    .viewportRow = 1,
                    .destinationWidthPixels =
                        static_cast<quint32>(layout->session.cellWidthPixels),
                    .destinationHeightPixels =
                        static_cast<quint32>(layout->session.cellHeightPixels),
                    .sourceWidth = 2,
                    .sourceHeight = 2,
                });
            }
            return snapshot;
        };
    controller->terminalUpdated(graphicsUpdate(
        makeImplicitSnapshot(red, {1, 2, 3},
                             TerminalKittyGraphicsLayer::AboveText),
        13));
    const TerminalPaneRenderProbeSnapshot implicit = paintedProbe();
    QCOMPARE(implicit.kittyGraphicsTextureUploadCount,
             overlapDeleted.kittyGraphicsTextureUploadCount + 1);
    QCOMPARE(implicit.kittyGraphicsTextureCount, qsizetype{1});
    QCOMPARE(implicit.kittyGraphicsNodeCreationCount,
             overlapDeleted.kittyGraphicsNodeCreationCount + 3);
    QCOMPARE(implicit.kittyGraphicsNodeDeletionCount,
             overlapDeleted.kittyGraphicsNodeDeletionCount);

    controller->terminalUpdated(
        graphicsUpdate(makeImplicitSnapshot(
                           red, {2, 3}, TerminalKittyGraphicsLayer::AboveText),
                       14));
    const TerminalPaneRenderProbeSnapshot implicitReduced = paintedProbe();
    QCOMPARE(implicitReduced.kittyGraphicsNodeCreationCount,
             implicit.kittyGraphicsNodeCreationCount);
    QCOMPARE(implicitReduced.kittyGraphicsNodeDeletionCount,
             implicit.kittyGraphicsNodeDeletionCount + 1);
    QCOMPARE(implicitReduced.kittyGraphicsGeometryWriteCount,
             implicit.kittyGraphicsGeometryWriteCount);
    QCOMPARE(implicitReduced.kittyGraphicsMaterialAssignmentCount,
             implicit.kittyGraphicsMaterialAssignmentCount);
    QCOMPARE(implicitReduced.kittyGraphicsTextureSetEvictionCount,
             implicit.kittyGraphicsTextureSetEvictionCount);

    controller->terminalUpdated(
        graphicsUpdate(makeImplicitSnapshot(
                           red, {3, 4}, TerminalKittyGraphicsLayer::AboveText),
                       15));
    const TerminalPaneRenderProbeSnapshot implicitMoved = paintedProbe();
    QCOMPARE(implicitMoved.kittyGraphicsNodeCreationCount,
             implicitReduced.kittyGraphicsNodeCreationCount);
    QCOMPARE(implicitMoved.kittyGraphicsNodeDeletionCount,
             implicitReduced.kittyGraphicsNodeDeletionCount);
    QCOMPARE(implicitMoved.kittyGraphicsGeometryWriteCount,
             implicitReduced.kittyGraphicsGeometryWriteCount + 1);
    QCOMPARE(implicitMoved.kittyGraphicsMaterialAssignmentCount,
             implicitReduced.kittyGraphicsMaterialAssignmentCount);

    controller->terminalUpdated(
        graphicsUpdate(makeImplicitSnapshot(
                           red, {3, 4}, TerminalKittyGraphicsLayer::BelowText),
                       16));
    const TerminalPaneRenderProbeSnapshot changedLayer = paintedProbe();
    QCOMPARE(changedLayer.kittyGraphicsTextureUploadCount,
             implicitMoved.kittyGraphicsTextureUploadCount);
    QCOMPARE(changedLayer.kittyGraphicsTextureCount, qsizetype{1});
    QCOMPARE(changedLayer.kittyGraphicsNodeCreationCount,
             implicitMoved.kittyGraphicsNodeCreationCount + 2);
    QCOMPARE(changedLayer.kittyGraphicsNodeDeletionCount,
             implicitMoved.kittyGraphicsNodeDeletionCount + 2);
    QCOMPARE(changedLayer.kittyGraphicsGeometryWriteCount,
             implicitMoved.kittyGraphicsGeometryWriteCount + 2);
    QCOMPARE(changedLayer.kittyGraphicsMaterialAssignmentCount,
             implicitMoved.kittyGraphicsMaterialAssignmentCount + 2);
    QCOMPARE(changedLayer.kittyGraphicsTextureSetEvictionCount,
             implicitMoved.kittyGraphicsTextureSetEvictionCount);

    controller->terminalUpdated(graphicsUpdate(makeSnapshot(nullptr), 17));
    const TerminalPaneRenderProbeSnapshot finalDeleted = paintedProbe();
    QCOMPARE(finalDeleted.kittyGraphicsNodeDeletionCount,
             changedLayer.kittyGraphicsNodeDeletionCount + 2);
    QCOMPARE(finalDeleted.kittyGraphicsTextureSetEvictionCount,
             changedLayer.kittyGraphicsTextureSetEvictionCount + 1);

    window.close();
    delete pane;
}

void TerminalPaneTest::reloadsShapingAndTracksLogicalCursorRows()
{
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    useSystemFixedFont(options);
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;

    constexpr int columns = 6;
    constexpr int rows = 2;
    const TerminalCellMetrics metrics = terminalCellMetrics(options.typography);
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(qCeil(metrics.cellWidth * columns),
                  qCeil(metrics.cellHeight * rows));
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy sessionEnded(pane, &TerminalPane::sessionEnded);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(sessionEnded.count(), 1, 5000);
    pane->forceActiveFocus();

    TerminalUpdate full;
    full.columns = columns;
    full.rows = rows;
    full.fullFrame = true;
    full.cursorChanged = true;
    full.cursorVisible = true;
    full.cursorBlinking = true;
    full.cursorStyle = 0; // Bar cursors do not recolor retained text.
    full.cursorColumn = 2;
    full.cursorRow = 0;
    full.contentRevision = 1;
    for (int row = 0; row < rows; ++row) {
        TerminalRowUpdate rowUpdate;
        rowUpdate.row = row;
        rowUpdate.cells.resize(columns);
        for (int column = 0; column < columns; ++column) {
            TerminalCell &cell = rowUpdate.cells[column];
            const QChar codepoint(
                static_cast<char16_t>(u'a' + row * columns + column));
            cell.text = QString(codepoint);
            cell.baseCodepoint = codepoint.unicode();
            cell.plainCodepoint = true;
            cell.foreground = Qt::white;
            cell.background = Qt::black;
        }
        full.dirtyRows.append(std::move(rowUpdate));
    }
    controller->terminalUpdated(full);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot initial =
        terminalPaneRenderProbe(pane);
    QCOMPARE(initial.rowLayoutCounts.size(), rows);
    QCOMPARE(initial.rowFallbackCellCounts.size(), rows);
    for (int row = 0; row < rows; ++row) {
        QVERIFY(initial.rowLayoutCounts.at(row) > 0);
        QVERIFY(initial.rowLayoutCounts.at(row) <= columns);
        QVERIFY(initial.rowFallbackCellCounts.at(row)
                <= initial.rowLayoutCounts.at(row));
    }
    // The cursor-free row must exercise the combined-layout fast path in the
    // deterministic offscreen test backend.
    QVERIFY(initial.rowLayoutCounts.at(1) < columns);
    QCOMPARE(initial.rowFallbackCellCounts.at(1), quint64{0});

    TerminalUpdate cursor;
    cursor.columns = columns;
    cursor.rows = rows;
    cursor.cursorChanged = true;
    cursor.cursorVisible = true;
    cursor.cursorBlinking = true;
    cursor.cursorStyle = 0;
    cursor.cursorColumn = 3;
    cursor.cursorRow = 1;
    cursor.contentRevision = 2;
    controller->terminalUpdated(cursor);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot moved = terminalPaneRenderProbe(pane);
    QCOMPARE(moved.rowBuildCounts.at(0), initial.rowBuildCounts.at(0) + 1);
    QCOMPARE(moved.rowBuildCounts.at(1), initial.rowBuildCounts.at(1) + 1);
    QCOMPARE(moved.rowSolidBuildCounts, initial.rowSolidBuildCounts);
    QCOMPARE(moved.solidCellVisitCount, initial.solidCellVisitCount);

    cursor.cursorColumn = 4;
    cursor.contentRevision = 3;
    controller->terminalUpdated(cursor);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot movedWithinRow =
        terminalPaneRenderProbe(pane);
    QCOMPARE(movedWithinRow.rowBuildCounts.at(0), moved.rowBuildCounts.at(0));
    QCOMPARE(movedWithinRow.rowBuildCounts.at(1),
             moved.rowBuildCounts.at(1) + 1);
    QCOMPARE(movedWithinRow.rowSolidBuildCounts, moved.rowSolidBuildCounts);

    LaunchOptions reloaded = options;
    reloaded.typography.shapingBreakCursor = false;
    constexpr quint32 Calt = 0x63616c74U;
    reloaded.typography.features = {
        {.tag = Calt, .value = 0},
    };
    pane->applyRuntimeOptions(reloaded);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot shapingDisabled =
        terminalPaneRenderProbe(pane);
    QCOMPARE(shapingDisabled.rootSerial, initial.rootSerial);
    QVERIFY(allVisibleRowsRebuilt(movedWithinRow, shapingDisabled));
    QCOMPARE(shapingDisabled.rowSolidBuildCounts,
             movedWithinRow.rowSolidBuildCounts);
    const auto calt = QFont::Tag::fromValue(Calt);
    QVERIFY(calt.has_value());
    QVERIFY(shapingDisabled.renderFonts
                .at(terminalEnumIndex(TerminalFontRole::Regular))
                .isFeatureSet(*calt));
    QCOMPARE(shapingDisabled.renderFonts
                 .at(terminalEnumIndex(TerminalFontRole::Regular))
                 .featureValue(*calt),
             quint32{0});

    cursor.cursorColumn = 1;
    cursor.cursorRow = 0;
    cursor.contentRevision = 4;
    controller->terminalUpdated(cursor);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot movedWhileDisabled =
        terminalPaneRenderProbe(pane);
    QCOMPARE(movedWhileDisabled.rowBuildCounts, shapingDisabled.rowBuildCounts);
    QCOMPARE(movedWhileDisabled.rowSolidBuildCounts,
             shapingDisabled.rowSolidBuildCounts);

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
    useSystemFixedFont(options);
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;

    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography);
    const qreal cellWidth = metrics.cellWidth;
    const qreal cellHeight = metrics.cellHeight;

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
    const TerminalCellMetrics secondWindowMetrics = terminalCellMetrics(
        options.typography, secondWindow.devicePixelRatio());
    const TerminalSessionGeometry resizedGeometry =
        resizeRequested.constLast()
            .constFirst()
            .value<TerminalSessionGeometry>();
    QCOMPARE(resizedGeometry.cellWidthPixels,
             qRound(secondWindowMetrics.cellWidth
                    * secondWindow.devicePixelRatio()));
    QCOMPARE(resizedGeometry.cellHeightPixels,
             qRound(secondWindowMetrics.cellHeight
                    * secondWindow.devicePixelRatio()));
    pane->update();
    const QImage rebuiltImage = secondWindow.grabWindow();
    QVERIFY(!rebuiltImage.isNull());
    const TerminalPaneRenderProbeSnapshot secondRoot =
        terminalPaneRenderProbe(pane);
    QVERIFY(secondRoot.metrics == secondWindowMetrics);
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

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
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

    QSignalSpy serializedRequests(
        controller, &TerminalController::serializedSearchRequested);
    QSignalSpy navigationRequests(
        controller, &TerminalController::searchNavigationRequested);
    QSignalSpy cancellationRequests(
        controller, &TerminalController::searchCancellationRequested);

    // Performability changes synchronously at typed-action dispatch time, so
    // adjacent search/navigation/end actions cannot observe a stale queued
    // worker acknowledgement.
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("search:immediate")));
    QVERIFY(controller->searchExpected());
    QVERIFY(
        pane.executeConfiguredAction(QStringLiteral("navigate_search:next")));
    QCOMPARE(navigationRequests.count(), 1);
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("search:")));
    QVERIFY(!controller->searchExpected());
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("navigate_search:previous")));
    QCOMPARE(navigationRequests.count(), 1);
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("end_search")));
    QVERIFY(!pane.searchUiActive());
    QCOMPARE(cancellationRequests.count(), 1);
    QCOMPARE(serializedRequests.count(), 2);

    // A malformed Ghostty escape is still forwarded so the worker sees the
    // ordered request, but it is synchronously inactive and unperformed.
    QVERIFY(!controller->searchSerialized(QByteArrayLiteral("\\x")));
    QVERIFY(!controller->searchExpected());
    QVERIFY(!controller->navigateSearch(TerminalSearchDirection::Next));
    QVERIFY(!controller->cancelSearch());

    // Empty search actions mirror Ghostty's performability while still
    // dispatching end_search so a stale frontend overlay is cleaned up.
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("search:")));
    pane.setSearchUiText(QString{});
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("start_search")));
    QVERIFY(pane.searchUiActive());
    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("end_search")));
    QVERIFY(!pane.searchUiActive());

    QSignalSpy searchSelection(
        controller, &TerminalController::searchSelectionActionRequested);
    QSignalSpy searchRequests(
        controller, &TerminalController::searchRequested);

    pane.endSearchUi();
    pane.setSearchUiText(QStringLiteral("retained query"));
    const int nonEmptySearchBefore = searchRequests.count();
    const int nonEmptyTextBefore = textSpy.count();
    const int nonEmptyFocusBefore = focusSpy.count();
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("search_selection")));
    QCOMPARE(searchSelection.count(), 1);
    const quint64 nonEmptyRequestId =
        searchSelection.constFirst().constFirst().toULongLong();
    const QString selectedText = QStringLiteral("  selected query  ");
    Q_EMIT controller->terminalActionReady({
        .requestId = nonEmptyRequestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::StartSearch,
        .performed = true,
        .payload = selectedText,
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    });
    QCoreApplication::processEvents();
    QVERIFY(pane.searchUiActive());
    QCOMPARE(pane.searchUiText(), selectedText);
    QCOMPARE(textSpy.count(), nonEmptyTextBefore + 1);
    QCOMPARE(focusSpy.count(), nonEmptyFocusBefore + 1);
    QCOMPARE(searchRequests.count(), nonEmptySearchBefore + 2);
    QCOMPARE(searchRequests.at(nonEmptySearchBefore).at(1).toByteArray(),
             QByteArrayLiteral("retained query"));
    QCOMPARE(searchRequests.at(nonEmptySearchBefore + 1).at(1).toByteArray(),
             selectedText.toUtf8());

    // A valid empty selection opens and focuses search while retaining the
    // previous entry text, matching Ghostty's GTK frontend sequencing.
    pane.endSearchUi();
    pane.setSearchUiText(QStringLiteral("retained empty selection"));
    const int emptySearchBefore = searchRequests.count();
    const int emptyTextBefore = textSpy.count();
    const int emptyFocusBefore = focusSpy.count();
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("search_selection")));
    QCOMPARE(searchSelection.count(), 2);
    const quint64 emptyRequestId =
        searchSelection.at(1).constFirst().toULongLong();
    Q_EMIT controller->terminalActionReady({
        .requestId = emptyRequestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::StartSearch,
        .performed = true,
        .payload = {},
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    });
    QCoreApplication::processEvents();
    QVERIFY(pane.searchUiActive());
    QCOMPARE(pane.searchUiText(),
             QStringLiteral("retained empty selection"));
    QCOMPARE(textSpy.count(), emptyTextBefore);
    QCOMPARE(focusSpy.count(), emptyFocusBefore + 1);
    QCOMPARE(searchRequests.count(), emptySearchBefore + 1);
    QCOMPARE(searchRequests.constLast().at(1).toByteArray(),
             QByteArrayLiteral("retained empty selection"));

    // An unavailable selection is a pure unperformed result: no overlay,
    // retained-text, focus, or backend-search mutation.
    pane.endSearchUi();
    const int unavailableSearchBefore = searchRequests.count();
    const int unavailableTextBefore = textSpy.count();
    const int unavailableFocusBefore = focusSpy.count();
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("search_selection")));
    QCOMPARE(searchSelection.count(), 3);
    const quint64 unavailableRequestId =
        searchSelection.at(2).constFirst().toULongLong();
    Q_EMIT controller->terminalActionReady({
        .requestId = unavailableRequestId,
        .outcome = TerminalActionOutcome::Unavailable,
        .effect = TerminalActionEffect::None,
        .performed = false,
    });
    QCoreApplication::processEvents();
    QVERIFY(!pane.searchUiActive());
    QCOMPARE(pane.searchUiText(),
             QStringLiteral("retained empty selection"));
    QCOMPARE(textSpy.count(), unavailableTextBefore);
    QCOMPARE(focusSpy.count(), unavailableFocusBefore);
    QCOMPARE(searchRequests.count(), unavailableSearchBefore);
}

void TerminalPaneTest::retainsStartingTextAcrossNoFramePaints()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    useSystemFixedFont(options);

    QQuickWindow window;
    window.resize(320, 160);
    auto *pane = new TerminalPane(options, window.contentItem(), std::nullopt,
                                  TerminalSessionStartMode::Deferred);
    pane->setSize(window.size());

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot first = terminalPaneRenderProbe(pane);
    QVERIFY(first.startingTextNodeSerial != 0);
    QVERIFY(first.startingTextBuildCount > 0);

    pane->update();
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot repeated =
        terminalPaneRenderProbe(pane);
    QCOMPARE(repeated.rootSerial, first.rootSerial);
    QCOMPARE(repeated.startingTextNodeSerial, first.startingTextNodeSerial);
    QCOMPARE(repeated.startingTextBuildCount, first.startingTextBuildCount);

    window.close();
    delete pane;
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
    options.typography.pointSize = 12.0;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy runtimeOptions(
        controller, &TerminalController::runtimeOptionsRequested);
    LaunchOptions reloaded = options;
    reloaded.typography.pointSize = 14.0;
    reloaded.typography.face(TerminalFontRole::Regular).families = {
        QStringLiteral("Monospace"),
    };
    reloaded.appearance.foregroundColor = QColor(QStringLiteral("#123456"));
    reloaded.scrollbackLimits = {
        .bytes = 321,
        .lines = 654,
    };
    reloaded.selectionClipboard = {
        .trimTrailingSpaces = false,
        .copyOnSelect = TerminalCopyOnSelectMode::PrimaryAndClipboard,
        .clearOnTyping = false,
        .clearOnCopy = true,
    };
    reloaded.selectionWordChars = {0, quint32{' '}, quint32{';'}};
    reloaded.clickRepeatIntervalMilliseconds = 731;
    reloaded.clipboardPaste = {
        .protection = false,
        .bracketedSafe = true,
    };
    reloaded.enquiryResponse = QByteArray::fromHex("00454e51ff");
    reloaded.middleClickAction = MiddleClickAction::Ignore;
    reloaded.mouseShiftCapture = MouseShiftCapture::Never;
    reloaded.vtKamAllowed = true;
    reloaded.linkUrl = false;
    reloaded.scrollbackCompression = false;
    reloaded.scrollToBottom = {
        .keystroke = false,
        .output = true,
    };
    reloaded.abnormalCommandExitRuntimeMilliseconds = 731;
    reloaded.waitAfterCommand = true;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.fontPointSize(), 14.0);
    const LaunchOptions splitOptions = pane.splitLaunchOptions(reloaded);
    QVERIFY(splitOptions.typography == reloaded.typography);
    QCOMPARE(splitOptions.typography.pointSize, 14.0);
    QCOMPARE(splitOptions.workingDirectory, QDir::tempPath());
    QVERIFY(splitOptions.program.isEmpty());
    QVERIFY(!splitOptions.hold);
    QCOMPARE(splitOptions.scrollbackLimits, reloaded.scrollbackLimits);
    QCOMPARE(splitOptions.appearance, reloaded.appearance);
    QCOMPARE(splitOptions.selectionClipboard, reloaded.selectionClipboard);
    QCOMPARE(splitOptions.selectionWordChars, reloaded.selectionWordChars);
    QCOMPARE(splitOptions.clickRepeatIntervalMilliseconds,
             reloaded.clickRepeatIntervalMilliseconds);
    QCOMPARE(splitOptions.clipboardPaste, reloaded.clipboardPaste);
    QCOMPARE(splitOptions.enquiryResponse, reloaded.enquiryResponse);
    QCOMPARE(splitOptions.middleClickAction, reloaded.middleClickAction);
    QCOMPARE(splitOptions.mouseShiftCapture, reloaded.mouseShiftCapture);
    QCOMPARE(splitOptions.vtKamAllowed, reloaded.vtKamAllowed);
    QCOMPARE(splitOptions.linkUrl, reloaded.linkUrl);
    QCOMPARE(splitOptions.scrollbackCompression,
             reloaded.scrollbackCompression);
    QCOMPARE(splitOptions.scrollToBottom, reloaded.scrollToBottom);
    QCOMPARE(splitOptions.abnormalCommandExitRuntimeMilliseconds,
             reloaded.abnormalCommandExitRuntimeMilliseconds);
    QCOMPARE(splitOptions.waitAfterCommand, reloaded.waitAfterCommand);
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
    QCOMPARE(
        fallback.typography.face(TerminalFontRole::Regular).families,
        QStringList({QStringLiteral("Monospace")}));
    QCOMPARE(fallback.typography.pointSize, 14.0);

    splitBase.splitInheritWorkingDirectory = true;
    const LaunchOptions directoryInherited =
        pane.splitLaunchOptions(splitBase);
    QCOMPARE(directoryInherited.workingDirectory, QDir::tempPath());
    QVERIFY(!directoryInherited.inheritWorkingDirectory);

    pane.zoomIn();
    QCOMPARE(pane.fontPointSize(), 15.0);
    reloaded.typography.pointSize = 10.0;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.fontPointSize(), 15.0);
    const LaunchOptions inherited = pane.splitLaunchOptions(reloaded);
    QCOMPARE(inherited.typography.pointSize, 15.0);

    LaunchOptions tabBase = reloaded;
    tabBase.workingDirectory = QDir::currentPath();
    tabBase.inheritWorkingDirectory = true;
    tabBase.typography.face(TerminalFontRole::Regular).families = {
        QStringLiteral("Configured Family"),
    };
    tabBase.tabInheritWorkingDirectory = false;
    tabBase.windowInheritFontSize = false;
    const LaunchOptions tabFallback = pane.tabLaunchOptions(tabBase);
    QCOMPARE(tabFallback.workingDirectory, QDir::currentPath());
    QVERIFY(tabFallback.inheritWorkingDirectory);
    QCOMPARE(
        tabFallback.typography.face(TerminalFontRole::Regular).families,
        QStringList({QStringLiteral("Configured Family")}));
    QCOMPARE(tabFallback.typography.pointSize, 10.0);
    QVERIFY(tabFallback.program.isEmpty());
    QVERIFY(!tabFallback.hold);

    tabBase.tabInheritWorkingDirectory = true;
    tabBase.windowInheritFontSize = true;
    const LaunchOptions tabInherited = pane.tabLaunchOptions(tabBase);
    QCOMPARE(tabInherited.workingDirectory, QDir::tempPath());
    QVERIFY(!tabInherited.inheritWorkingDirectory);
    QCOMPARE(
        tabInherited.typography.face(TerminalFontRole::Regular).families,
        QStringList({QStringLiteral("Configured Family")}));
    QCOMPARE(tabInherited.typography.pointSize, 15.0);

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
    nextReload.typography.pointSize = 9.0;
    pane.applyRuntimeOptions(nextReload);
    child.applyRuntimeOptions(nextReload);
    QCOMPARE(pane.fontPointSize(), 15.0);
    QCOMPARE(child.fontPointSize(), 9.0);

    pane.resetZoom();
    QCOMPARE(pane.fontPointSize(), 9.0);
    nextReload.typography.pointSize = 11.0;
    pane.applyRuntimeOptions(nextReload);
    child.applyRuntimeOptions(nextReload);
    QCOMPARE(pane.fontPointSize(), 11.0);
    QCOMPARE(child.fontPointSize(), 11.0);
}

void TerminalPaneTest::reloadsTypographyNonCumulatively()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 5"),
    };
    options.hold = true;
    useSystemFixedFont(options);
    options.typography.pointSize = 12.0;

    QQuickWindow window;
    window.resize(800, 400);
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy resized(controller, &TerminalController::resizeRequested);
    QSignalSpy fontSizeChanged(pane, &TerminalPane::fontPointSizeChanged);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QVERIFY(!window.grabWindow().isNull());
    resized.clear();

    LaunchOptions first = options;
    first.typography.metricModifiers[TerminalMetric::CellWidth] =
        TerminalMetricModifiers::Absolute{.pixels = 1};
    pane->applyRuntimeOptions(first);
    QCOMPARE(resized.count(), 1);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(
        terminalPaneRenderProbe(pane).metrics
        == terminalCellMetrics(
            first.typography, window.devicePixelRatio()));

    // Replacing +1 with +2 derives from the regular face again; it must not
    // accumulate into +3 across live reloads.
    resized.clear();
    LaunchOptions second = first;
    second.typography.metricModifiers[TerminalMetric::CellWidth] =
        TerminalMetricModifiers::Absolute{.pixels = 2};
    pane->applyRuntimeOptions(second);
    QCOMPARE(resized.count(), 1);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(
        terminalPaneRenderProbe(pane).metrics
        == terminalCellMetrics(
            second.typography, window.devicePixelRatio()));

    // Non-grid metrics rebuild rendering but do not publish redundant PTY
    // geometry.
    resized.clear();
    LaunchOptions baselineOnly = second;
    baselineOnly.typography.metricModifiers[TerminalMetric::FontBaseline] =
        TerminalMetricModifiers::Absolute{.pixels = 3};
    pane->applyRuntimeOptions(baselineOnly);
    QCOMPARE(resized.count(), 0);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(
        terminalPaneRenderProbe(pane).metrics
        == terminalCellMetrics(
            baselineOnly.typography, window.devicePixelRatio()));

    pane->zoomIn();
    QCOMPARE(pane->fontPointSize(), 13.0);
    fontSizeChanged.clear();
    resized.clear();

    // A zoomed pane retains only its current point size. Every configured
    // face, style, and modifier is replaced by the newest snapshot.
    LaunchOptions reloaded = baselineOnly;
    reloaded.typography.pointSize = 20.0;
    reloaded.typography.face(TerminalFontRole::Bold).style =
        TerminalFontStyles::Disabled{};
    reloaded.typography.metricModifiers[TerminalMetric::CellWidth] =
        TerminalMetricModifiers::Absolute{.pixels = 4};
    reloaded.typography.metricModifiers[TerminalMetric::UnderlinePosition] =
        TerminalMetricModifiers::Absolute{.pixels = 2};
    pane->applyRuntimeOptions(reloaded);
    QCOMPARE(pane->fontPointSize(), 13.0);
    QCOMPARE(fontSizeChanged.count(), 0);
    QCOMPARE(resized.count(), 1);
    TerminalTypography zoomedTypography = reloaded.typography;
    zoomedTypography.pointSize = 13.0;
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(
        terminalPaneRenderProbe(pane).metrics
        == terminalCellMetrics(
            zoomedTypography, window.devicePixelRatio()));
    const LaunchOptions split = pane->splitLaunchOptions(reloaded);
    QVERIFY(split.typography == zoomedTypography);

    resized.clear();
    pane->resetZoom();
    QCOMPARE(pane->fontPointSize(), 20.0);
    QCOMPARE(fontSizeChanged.count(), 1);
    QCOMPARE(resized.count(), 1);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(
        terminalPaneRenderProbe(pane).metrics
        == terminalCellMetrics(
            reloaded.typography, window.devicePixelRatio()));

    // A source-level modifier that produces the same finalized metrics is a
    // render/geometry no-op.
    resized.clear();
    LaunchOptions equivalent = reloaded;
    equivalent.typography.metricModifiers[
        TerminalMetric::OverlineThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 0};
    pane->applyRuntimeOptions(equivalent);
    QCOMPARE(resized.count(), 0);
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(
        terminalPaneRenderProbe(pane).metrics
        == terminalCellMetrics(
            equivalent.typography, window.devicePixelRatio()));

    window.close();
    delete pane;
}

void TerminalPaneTest::refreshesFontProgramAfterDatabaseChange()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    useSystemFixedFont(options);

    QQuickWindow window;
    window.resize(320, 160);
    auto *pane = new TerminalPane(options, window.contentItem(), std::nullopt,
                                  TerminalSessionStartMode::Deferred);
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QVERIFY(!window.grabWindow().isNull());
    const std::shared_ptr<const TerminalFontProgram> initial =
        terminalPaneRenderProbe(pane).metrics.fontProgram;
    QVERIFY(initial != nullptr);

    const QString path =
        QFINDTESTDATA("../ghostty/src/font/res/CodeNewRoman-Regular.otf");
    QVERIFY2(!path.isEmpty(), "Bundled font was not found");
    const int fontId = QFontDatabase::addApplicationFont(path);
    QVERIFY(fontId >= 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        !window.grabWindow().isNull()
            && terminalPaneRenderProbe(pane).metrics.fontProgram != initial,
        3000);
    const std::shared_ptr<const TerminalFontProgram> added =
        terminalPaneRenderProbe(pane).metrics.fontProgram;
    QCOMPARE(pane->findChild<TerminalController *>(), controller);

    QVERIFY(QFontDatabase::removeApplicationFont(fontId));
    QTRY_VERIFY_WITH_TIMEOUT(
        !window.grabWindow().isNull()
            && terminalPaneRenderProbe(pane).metrics.fontProgram != added,
        3000);
    QCOMPARE(pane->findChild<TerminalController *>(), controller);

    window.close();
    delete pane;
}

void TerminalPaneTest::executesTypedFontSizeActions()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.typography.pointSize = 12.0;

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
    reloaded.typography.pointSize = 10.0;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(pane.fontPointSize(), 12.0);

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("reset_font_size")));
    QCOMPARE(pane.fontPointSize(), 10.0);
    reloaded.typography.pointSize = 11.0;
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
    const TerminalSessionGeometry tiny =
        resized.constLast().constFirst().value<TerminalSessionGeometry>();
    QCOMPARE(tiny.columns, 1);
    QCOMPARE(tiny.rows, 1);

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
    oversized.typography.pointSize = 300.0;
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
    oversizedReload.typography.pointSize = 300.0;
    reloadBounds.applyRuntimeOptions(oversizedReload);
    QCOMPARE(reloadBounds.fontPointSize(), 255.0);
    reloadBounds.resetZoom();
    QCOMPARE(reloadBounds.fontPointSize(), 300.0);
}

void TerminalPaneTest::workspaceActionHandlerRetainsMutableState()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    int totalCalls = 0;
    pane.setWorkspaceActionHandler(
        [calls = 0, &totalCalls](WorkspaceActionRequest) mutable {
            ++totalCalls;
            return ++calls == 2;
        });

    QVERIFY(!pane.executeConfiguredAction(QStringLiteral("new_tab")));
    QVERIFY(pane.executeConfiguredAction(QStringLiteral("new_tab")));
    QCOMPARE(totalCalls, 2);
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

void TerminalPaneTest::configuredTitleReapplicationRestoresBaseLayer()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.configuredTitle = QStringLiteral("configured base");

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    auto *const controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QCOMPARE(controller->title(), QStringLiteral("configured base"));
    QCOMPARE(pane.title(), QStringLiteral("configured base"));

    QSignalSpy titleChanges(&pane, &TerminalPane::titleChanged);
    pane.setSurfaceTitle(QStringLiteral("action base"));
    QCOMPARE(controller->title(), QStringLiteral("action base"));
    QCOMPARE(pane.title(), QStringLiteral("action base"));

    // An identical successful reload is still a configured-title write and
    // therefore replaces a base changed by set_surface_title.
    pane.applyRuntimeOptions(options);
    QCOMPARE(controller->title(), QStringLiteral("configured base"));
    QCOMPARE(pane.title(), QStringLiteral("configured base"));

    const QString override = QStringLiteral("persistent override");
    pane.setSurfaceTitleOverride(override);
    pane.setSurfaceTitle(QStringLiteral("hidden action base"));
    QCOMPARE(controller->title(), QStringLiteral("hidden action base"));
    QCOMPARE(pane.title(), override);
    pane.applyRuntimeOptions(options);
    QCOMPARE(controller->title(), QStringLiteral("configured base"));
    QCOMPARE(pane.title(), override);

    LaunchOptions cleared = options;
    cleared.configuredTitle.reset();
    pane.applyRuntimeOptions(cleared);
    QCOMPARE(controller->title(), QStringLiteral("configured base"));
    QCOMPARE(pane.title(), override);
    pane.setSurfaceTitleOverride(std::nullopt);
    QCOMPARE(pane.title(), QStringLiteral("configured base"));

    LaunchOptions empty = cleared;
    empty.configuredTitle = QString{};
    pane.applyRuntimeOptions(empty);
    QVERIFY(controller->hasTitle());
    QVERIFY(controller->title().isEmpty());
    QVERIFY(pane.title().isEmpty());
    QVERIFY(titleChanges.count() >= 4);

    LaunchOptions outer = empty;
    outer.configuredTitle = QStringLiteral("outer title");
    LaunchOptions nested = empty;
    nested.configuredTitle = QStringLiteral("nested title");
    bool reentered = false;
    const QMetaObject::Connection connection =
        connect(&pane, &TerminalPane::titleChanged, &pane, [&] {
            if (reentered) return;
            reentered = true;
            pane.applyRuntimeOptions(nested);
        });
    pane.applyRuntimeOptions(outer);
    disconnect(connection);
    QVERIFY(reentered);
    QCOMPARE(controller->title(), QStringLiteral("nested title"));
    QCOMPARE(pane.title(), QStringLiteral("nested title"));
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

    // Ghostty installs an exact direct argv[0] base title. It is intentionally
    // not basename-normalized, and copy_title consumes that raw surface layer.
    resetClipboards();
    QCOMPARE(pane.title(), QStringLiteral("/bin/true"));
    QCOMPARE(pane.effectiveSurfaceTitle(),
             std::optional<QString>{QStringLiteral("/bin/true")});
    QVERIFY(pane.executeConfiguredAction(copyAction));
    QCOMPARE(clipboard->text(QClipboard::Clipboard),
             QStringLiteral("/bin/true"));
    verifyPrimaryUnchanged();

    // Structured direct commands require an argv entry but deliberately retain
    // an empty argv[0] for byte-exact transport and safe launch failure. The
    // title layer preserves that explicit empty separately from absence.
    LaunchOptions emptyDirectOptions = options;
    emptyDirectOptions.program.clear();
    emptyDirectOptions.ordinaryCommand =
        TerminalCommand::direct({QByteArray{}});
    TerminalPane emptyDirectPane(emptyDirectOptions, nullptr, std::nullopt,
                                 TerminalSessionStartMode::Deferred);
    QVERIFY(emptyDirectPane.title().isEmpty());
    QCOMPARE(emptyDirectPane.effectiveSurfaceTitle(),
             std::optional<QString>{QString{}});
    QVERIFY(!emptyDirectPane.executeConfiguredAction(copyAction));

    LaunchOptions shellOptions = options;
    shellOptions.program.clear();
    shellOptions.ordinaryCommand =
        TerminalCommand::shell(QByteArrayLiteral("exec /bin/true"));
    TerminalPane shellPane(shellOptions, nullptr, std::nullopt,
                           TerminalSessionStartMode::Deferred);
    QCOMPARE(shellPane.title(), QStringLiteral("Terminal"));
    QVERIFY(!shellPane.effectiveSurfaceTitle().has_value());
    QVERIFY(!shellPane.executeConfiguredAction(copyAction));

    LaunchOptions structuredOptions = options;
    structuredOptions.program.clear();
    structuredOptions.ordinaryCommand = TerminalCommand::direct(
        {QByteArrayLiteral("/bin/sleep"), QByteArrayLiteral("1")});
    TerminalPane structuredPane(structuredOptions, nullptr, std::nullopt,
                                TerminalSessionStartMode::Deferred);
    QCOMPARE(structuredPane.effectiveSurfaceTitle(),
             std::optional<QString>{QStringLiteral("/bin/sleep")});

    // An explicit empty base remains a title-layer value, but upstream treats
    // it as a copy no-op just like absence.
    resetClipboards();
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

void TerminalPaneTest::routesConfiguredRightClickPolicy()
{
    qRegisterMetaType<TerminalRightClickInput>();
    qRegisterMetaType<TerminalRightClickResult>();

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("printf '\\033[?1000hright-ready'; sleep 5"),
    };
    options.hold = true;

    QQuickWindow window;
    window.resize(640, 480);
    QQuickItem container(window.contentItem());
    container.setPosition(QPointF(31.0, 43.0));
    container.setSize(QSizeF(500.0, 300.0));
    TerminalPane pane(options, &container);
    pane.setPosition(QPointF(7.0, 11.0));
    pane.setSize(QSizeF(320.0, 160.0));
    window.show();

    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy mouseRequests(controller, &TerminalController::mouseRequested);
    QSignalSpy rightClickRequests(controller,
                                  &TerminalController::rightClickRequested);
    QSignalSpy menuRequests(&pane, &TerminalPane::contextMenuRequested);
    QSignalSpy pasteRequests(controller, &TerminalController::pasteRequested);
    QSignalSpy runtimeRequests(controller,
                               &TerminalController::runtimeOptionsRequested);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("right-ready")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->mouseTracking(), 1000);

    QStringList lifecycle;
    connect(&pane, &TerminalPane::activated, &pane,
            [&lifecycle] { lifecycle.append(QStringLiteral("activated")); });
    connect(controller, &TerminalController::rightClickRequested, &pane,
            [&lifecycle] { lifecycle.append(QStringLiteral("right-click")); });

    const QPointF localPosition(17.0, 23.0);
    const QPointF expectedWindowPosition =
        pane.mapToItem(window.contentItem(), localPosition);
    const auto sendRight = [&](QEvent::Type type,
                               Qt::KeyboardModifiers modifiers) {
        const Qt::MouseButtons buttons =
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::RightButton;
        QMouseEvent event(type, localPosition, localPosition, localPosition,
                          Qt::RightButton, buttons, modifiers);
        QCoreApplication::sendEvent(&pane, &event);
        QVERIFY(event.isAccepted());
    };
    const auto rightClickInputAt = [&rightClickRequests](qsizetype index) {
        return qvariant_cast<TerminalRightClickInput>(
            rightClickRequests.at(index).constFirst());
    };

    sendRight(QEvent::MouseButtonPress, Qt::NoModifier);
    QCOMPARE(mouseRequests.size(), 1);
    QCOMPARE(rightClickRequests.size(), 0);

    lifecycle.clear();
    sendRight(QEvent::MouseButtonPress,
              Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(mouseRequests.size(), 1);
    QCOMPARE(rightClickRequests.size(), 1);
    QVERIFY(lifecycle.contains(QStringLiteral("activated")));
    QVERIFY(lifecycle.contains(QStringLiteral("right-click")));
    QVERIFY(lifecycle.indexOf(QStringLiteral("activated"))
            < lifecycle.indexOf(QStringLiteral("right-click")));
    const TerminalRightClickInput first = rightClickInputAt(0);
    QVERIFY(first.requestId != 0);
    QVERIFY(first.contentRevision != 0);
    QVERIFY(first.modifiers & Qt::ControlModifier);
    QVERIFY(first.modifiers & Qt::ShiftModifier);
    QVERIFY(first.shiftBypassedMouseCapture);

    Q_EMIT controller->rightClickResolved({
        .requestId = first.requestId,
        .effect = TerminalRightClickEffect::ContextMenu,
        .selectionAvailable = true,
    });
    QCOMPARE(menuRequests.size(), 1);
    QCOMPARE(menuRequests.constFirst().at(0).toPointF(),
             expectedWindowPosition);
    QCOMPARE(menuRequests.constFirst().at(1).toBool(), true);

    // Release has no local action, and an older completion cannot reuse the
    // popup position retained for a newer press.
    sendRight(QEvent::MouseButtonRelease,
              Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(rightClickRequests.size(), 1);
    sendRight(QEvent::MouseButtonPress, Qt::ShiftModifier);
    sendRight(QEvent::MouseButtonPress, Qt::ShiftModifier);
    QCOMPARE(rightClickRequests.size(), 3);
    const TerminalRightClickInput superseded = rightClickInputAt(1);
    const TerminalRightClickInput current = rightClickInputAt(2);
    Q_EMIT controller->rightClickResolved({
        .requestId = superseded.requestId,
        .effect = TerminalRightClickEffect::ContextMenu,
        .selectionAvailable = false,
    });
    QCOMPARE(menuRequests.size(), 1);
    Q_EMIT controller->rightClickResolved({
        .requestId = current.requestId,
        .effect = TerminalRightClickEffect::ContextMenu,
        .selectionAvailable = false,
    });
    QCOMPARE(menuRequests.size(), 2);
    QCOMPARE(menuRequests.constLast().at(0).toPointF(), expectedWindowPosition);
    QCOMPARE(menuRequests.constLast().at(1).toBool(), false);

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    clipboard->setText(QStringLiteral("right-click-paste"),
                       QClipboard::Clipboard);
    sendRight(QEvent::MouseButtonPress, Qt::ShiftModifier);
    const TerminalRightClickInput paste = rightClickInputAt(3);
    Q_EMIT controller->rightClickResolved({
        .requestId = paste.requestId,
        .effect = TerminalRightClickEffect::Paste,
    });
    QCOMPARE(pasteRequests.size(), 1);
    QCOMPARE(pasteRequests.constFirst().constFirst().toString(),
             QStringLiteral("right-click-paste"));
    QCOMPARE(menuRequests.size(), 2);

    // Raw DEC capture still strips the Shift escape modifier when the
    // mouse-reporting policy routes the physical press locally. Independent
    // in-flight paste effects must both survive popup supersession.
    LaunchOptions reloaded = options;
    reloaded.mouseReporting = false;
    reloaded.rightClickAction = RightClickAction::Paste;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(runtimeRequests.size(), 1);
    const TerminalSessionRuntimeOptions pastedRuntime =
        qvariant_cast<TerminalSessionRuntimeOptions>(
            runtimeRequests.constFirst().constFirst());
    QCOMPARE(pastedRuntime.rightClickAction, RightClickAction::Paste);
    QVERIFY(controller->terminalMouseTracking());
    QVERIFY(!controller->mouseTracking());

    sendRight(QEvent::MouseButtonPress,
              Qt::ControlModifier | Qt::ShiftModifier);
    sendRight(QEvent::MouseButtonPress,
              Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(rightClickRequests.size(), 6);
    QVERIFY(rightClickInputAt(4).shiftBypassedMouseCapture);
    QVERIFY(rightClickInputAt(5).shiftBypassedMouseCapture);
    QTRY_COMPARE_WITH_TIMEOUT(pasteRequests.size(), 3, 1000);

    reloaded.rightClickAction = RightClickAction::Ignore;
    pane.applyRuntimeOptions(reloaded);
    QCOMPARE(runtimeRequests.size(), 2);
    QCOMPARE(qvariant_cast<TerminalSessionRuntimeOptions>(
                 runtimeRequests.constLast().constFirst())
                 .rightClickAction,
             RightClickAction::Ignore);

    clipboard->clear(QClipboard::Clipboard);
}

void TerminalPaneTest::scalesAndAccumulatesDiscreteWheelInputAcrossReloads()
{
    qRegisterMetaType<TerminalWheelInput>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    TerminalPane *const identity = &pane;
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    TerminalController *const controllerIdentity = controller;
    QSignalSpy wheel(controller, &TerminalController::wheelRequested);
    QSignalSpy mouse(controller, &TerminalController::mouseRequested);

    const auto requestAt = [&wheel](int index) {
        return qvariant_cast<TerminalWheelInput>(wheel.at(index).constFirst());
    };
    const auto sendAngle = [&pane](int delta) {
        return sendWheelEvent(pane, QPoint{}, QPoint(0, delta));
    };

    // One ordinary notch uses Ghostty's discrete default of three rows.
    QVERIFY(sendAngle(120));
    QCOMPARE(wheel.count(), 1);
    QCOMPARE(requestAt(0).rows, qint64{3});
    QVERIFY(requestAt(0).mouseReportingEnabled);
    QVERIFY(sendAngle(-120));
    QCOMPARE(wheel.count(), 2);
    QCOMPARE(requestAt(1).rows, qint64{-3});
    QCOMPARE(mouse.count(), 0);

    // Fractional non-precision ticks accumulate as signed physical distance.
    // Opposite movement cancels the retained half-row exactly.
    LaunchOptions fractional = options;
    fractional.mouseScrollMultiplier.discrete = 2.0;
    pane.applyRuntimeOptions(fractional);
    QVERIFY(sendAngle(30));
    QVERIFY(sendAngle(-30));
    QCOMPARE(wheel.count(), 2);

    QVERIFY(sendAngle(30));
    QCOMPARE(wheel.count(), 2);
    QVERIFY(sendAngle(30));
    QCOMPARE(wheel.count(), 3);
    QCOMPARE(requestAt(2).rows, qint64{1});
    QVERIFY(sendAngle(-30));
    QCOMPARE(wheel.count(), 3);
    QVERIFY(sendAngle(-30));
    QCOMPARE(wheel.count(), 4);
    QCOMPARE(requestAt(3).rows, qint64{-1});

    // Reload changes only future contributions. The half-row retained from
    // the first 1.5-row notch combines with the next 0.5-row notch.
    LaunchOptions oneAndAHalf = fractional;
    oneAndAHalf.mouseScrollMultiplier.discrete = 1.5;
    pane.applyRuntimeOptions(oneAndAHalf);
    QVERIFY(sendAngle(120));
    QCOMPARE(wheel.count(), 5);
    QCOMPARE(requestAt(4).rows, qint64{1});

    LaunchOptions oneHalf = oneAndAHalf;
    oneHalf.mouseScrollMultiplier.discrete = 0.5;
    pane.applyRuntimeOptions(oneHalf);
    QCOMPARE(&pane, identity);
    QCOMPARE(pane.findChild<TerminalController *>(), controllerIdentity);
    QVERIFY(sendAngle(120));
    QCOMPARE(wheel.count(), 6);
    QCOMPARE(requestAt(5).rows, qint64{1});

    // A coalesced extreme cannot monopolize the GUI with an unbounded report
    // loop. One dispatch is capped at the maximum finalized single-notch
    // multiplier, and the retained debt still cancels against later motion.
    LaunchOptions extreme = oneHalf;
    extreme.mouseScrollMultiplier.discrete = 10'000.0;
    pane.applyRuntimeOptions(extreme);
    QVERIFY(sendAngle(240));
    QCOMPARE(wheel.count(), 7);
    QCOMPARE(requestAt(6).rows, qint64{10'000});
    QVERIFY(sendAngle(-120));
    QCOMPARE(wheel.count(), 7);
}

void TerminalPaneTest::prefersPrecisionPixelsAndRetainsPhysicalWheelDistance()
{
    qRegisterMetaType<TerminalWheelInput>();

    QQuickWindow window;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    useSystemFixedFont(options);

    const qreal dpr = window.devicePixelRatio();
    const TerminalCellMetrics baseMetrics =
        terminalCellMetrics(options.typography, dpr);
    const qint32 basePhysicalCellHeight = qRound(baseMetrics.cellHeight * dpr);
    QVERIFY(basePhysicalCellHeight > 0);
    constexpr int logicalPixelDelta = 4;
    options.mouseScrollMultiplier.precision =
        static_cast<double>(basePhysicalCellHeight)
        / (2.0 * logicalPixelDelta * dpr);

    auto *pane = new TerminalPane(options, window.contentItem(), std::nullopt,
                                  TerminalSessionStartMode::Deferred);
    pane->setSize(QSizeF(320.0, 160.0));
    TerminalPane *const identity = pane;
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    TerminalController *const controllerIdentity = controller;
    QSignalSpy wheel(controller, &TerminalController::wheelRequested);

    const auto requestAt = [&wheel](int index) {
        return qvariant_cast<TerminalWheelInput>(wheel.at(index).constFirst());
    };
    const auto sendPixel = [pane](int pixelDelta, int contraryAngleDelta) {
        return sendWheelEvent(*pane, QPoint(0, pixelDelta),
                              QPoint(0, contraryAngleDelta));
    };

    // Each pixel event contributes exactly half of the original physical cell
    // height. The deliberately contrary angle delta proves pixelDelta wins,
    // and opposite precision movement cancels without a viewport request.
    QVERIFY(sendPixel(logicalPixelDelta, -120));
    QVERIFY(sendPixel(-logicalPixelDelta, 120));
    QCOMPARE(wheel.count(), 0);
    QVERIFY(sendPixel(logicalPixelDelta, -120));
    QCOMPARE(wheel.count(), 0);

    // Qt does not expose a mutable test DPR, but this is the exact event used
    // by the pane to refresh DPR-derived metrics. Retained device-pixel
    // distance must survive that refresh.
    QEvent dprChange(QEvent::DevicePixelRatioChange);
    QCoreApplication::sendEvent(&window, &dprChange);

    // Double the physical cell height. The retained half-old-cell is now one
    // quarter of the current cell; three more identical contributions reach
    // one new cell. Resetting or normalizing the pending distance on reload
    // would fail this boundary.
    LaunchOptions tall = options;
    tall.typography.metricModifiers[TerminalMetric::CellHeight] =
        TerminalMetricModifiers::Absolute{
            .pixels = basePhysicalCellHeight,
        };
    const TerminalCellMetrics tallMetrics =
        terminalCellMetrics(tall.typography, dpr);
    const qint32 tallPhysicalCellHeight = qRound(tallMetrics.cellHeight * dpr);
    QCOMPARE(tallPhysicalCellHeight, basePhysicalCellHeight * 2);
    pane->applyRuntimeOptions(tall);
    QCOMPARE(pane, identity);
    QCOMPARE(pane->findChild<TerminalController *>(), controllerIdentity);

    QVERIFY(sendPixel(logicalPixelDelta, -120));
    QVERIFY(sendPixel(logicalPixelDelta, -120));
    QCOMPARE(wheel.count(), 0);
    QVERIFY(sendPixel(logicalPixelDelta, -120));
    QCOMPARE(wheel.count(), 1);
    QCOMPARE(requestAt(0).rows, qint64{1});

    // A live precision reload affects the next contribution without replacing
    // the pane or controller. One logical delta now equals one tall cell, with
    // exact negative symmetry.
    LaunchOptions fullCell = tall;
    fullCell.mouseScrollMultiplier.precision =
        static_cast<double>(tallPhysicalCellHeight) / (logicalPixelDelta * dpr);
    pane->applyRuntimeOptions(fullCell);
    QCOMPARE(pane, identity);
    QCOMPARE(pane->findChild<TerminalController *>(), controllerIdentity);
    QVERIFY(sendPixel(logicalPixelDelta, -120));
    QCOMPARE(wheel.count(), 2);
    QCOMPARE(requestAt(1).rows, qint64{1});
    QVERIFY(sendPixel(-logicalPixelDelta, 120));
    QCOMPARE(wheel.count(), 3);
    QCOMPARE(requestAt(2).rows, qint64{-1});

    delete pane;
}

void TerminalPaneTest::normalizesHorizontalWheelInputIndependently()
{
    qRegisterMetaType<TerminalWheelInput>();

    QQuickWindow window;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    useSystemFixedFont(options);
    options.horizontalTabScroll = false;
    // This multiplier must affect only vertical precision movement.
    options.mouseScrollMultiplier.precision = 10'000.0;

    const qreal dpr = window.devicePixelRatio();
    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography, dpr);
    const qint32 physicalCellWidth = qRound(metrics.cellWidth * dpr);
    QVERIFY(physicalCellWidth > dpr);
    const int horizontalPiece =
        static_cast<int>(std::floor((physicalCellWidth - 1) / dpr));
    QVERIFY(horizontalPiece > 0);

    auto *pane = new TerminalPane(options, window.contentItem(), std::nullopt,
                                  TerminalSessionStartMode::Deferred);
    pane->setSize(QSizeF(320.0, 160.0));
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy wheel(controller, &TerminalController::wheelRequested);
    const auto requestAt = [&wheel](int index) {
        return qvariant_cast<TerminalWheelInput>(wheel.at(index).constFirst());
    };

    // Precision X retains raw physical distance against cell width. The
    // deliberately extreme vertical multiplier and contrary angle delta do
    // not affect it.
    QVERIFY(
        sendWheelEvent(*pane, QPoint(horizontalPiece, 0), QPoint(-120, 120)));
    QCOMPARE(wheel.count(), 0);
    QVERIFY(
        sendWheelEvent(*pane, QPoint(horizontalPiece, 0), QPoint(-120, 120)));
    QCOMPARE(wheel.count(), 1);
    QCOMPARE(requestAt(0).rows, qint64{0});
    QCOMPARE(requestAt(0).columns, qint64{1});
    QVERIFY(sendWheelEvent(*pane, QPoint(-horizontalPiece, 0), QPoint{}));
    QCOMPARE(wheel.count(), 1);
    QVERIFY(sendWheelEvent(*pane, QPoint(-horizontalPiece, 0), QPoint{}));
    QCOMPARE(wheel.count(), 2);
    QCOMPARE(requestAt(1).columns, qint64{-1});

    // Discrete X rounds each event independently rather than accumulating
    // fractional ticks, and remains independent of the vertical multiplier.
    QVERIFY(sendWheelEvent(*pane, QPoint{}, QPoint(59, 0)));
    QCOMPARE(wheel.count(), 2);
    QVERIFY(sendWheelEvent(*pane, QPoint{}, QPoint(60, 0)));
    QCOMPARE(wheel.count(), 3);
    QCOMPARE(requestAt(2).rows, qint64{0});
    QCOMPARE(requestAt(2).columns, qint64{1});
    QVERIFY(sendWheelEvent(*pane, QPoint{}, QPoint(-60, 0)));
    QCOMPARE(wheel.count(), 4);
    QCOMPARE(requestAt(3).columns, qint64{-1});

    // A diagonal discrete event crosses the session boundary once with both
    // independently normalized axes. Synthesized X extremes remain bounded.
    LaunchOptions ordinary = options;
    ordinary.mouseScrollMultiplier.discrete = 1.0;
    pane->applyRuntimeOptions(ordinary);
    QVERIFY(sendWheelEvent(*pane, QPoint{}, QPoint(120, 120)));
    QCOMPARE(wheel.count(), 5);
    QCOMPARE(requestAt(4).rows, qint64{1});
    QCOMPARE(requestAt(4).columns, qint64{1});
    QVERIFY(sendWheelEvent(*pane, QPoint{},
                           QPoint(std::numeric_limits<int>::max(), 0)));
    QCOMPARE(wheel.count(), 6);
    QCOMPARE(requestAt(5).rows, qint64{0});
    QCOMPARE(requestAt(5).columns, qint64{10'000});

    delete pane;
}

void TerminalPaneTest::switchesTabsFromPrecisionHorizontalScroll()
{
    qRegisterMetaType<TerminalWheelInput>();

    QQuickWindow window;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.horizontalTabScroll = true;
    useSystemFixedFont(options);

    auto *pane = new TerminalPane(options, window.contentItem(), std::nullopt,
                                  TerminalSessionStartMode::Deferred);
    pane->setSize(QSizeF(320.0, 160.0));
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy tabChange(pane, &TerminalPane::requestTabChange);
    QSignalSpy wheel(controller, &TerminalController::wheelRequested);
    const auto requestAt = [&wheel](int index) {
        return qvariant_cast<TerminalWheelInput>(wheel.at(index).constFirst());
    };
    const auto sendPixel = [pane](int horizontal, int vertical = 0) {
        return sendWheelEvent(*pane, QPoint(horizontal, vertical), QPoint{});
    };

    // A partial touchpad gesture expires after 500 ms. The next two halves
    // therefore cross the threshold only after accumulating together.
    QVERIFY(sendPixel(-60));
    QCOMPARE(tabChange.count(), 0);
    QCOMPARE(wheel.count(), 0);
    QTest::qWait(600);
    QVERIFY(sendPixel(-60));
    QCOMPARE(tabChange.count(), 0);
    QVERIFY(sendPixel(-60));
    QCOMPARE(tabChange.count(), 1);
    QCOMPARE(tabChange.constLast().constFirst().toInt(), 1);
    QCOMPARE(wheel.count(), 0);

    // Positive movement selects the previous tab. A coalesced gesture changes
    // at most one tab and discards excess distance.
    QVERIFY(sendPixel(120));
    QCOMPARE(tabChange.count(), 2);
    QCOMPARE(tabChange.constLast().constFirst().toInt(), -1);
    QVERIFY(sendPixel(-240));
    QCOMPARE(tabChange.count(), 3);
    QCOMPARE(tabChange.constLast().constFirst().toInt(), 1);
    QCOMPARE(wheel.count(), 0);

    // Opposite subthreshold movement cancels without reaching the terminal.
    QVERIFY(sendPixel(60));
    QVERIFY(sendPixel(-60));
    QCOMPARE(tabChange.count(), 3);
    QCOMPARE(wheel.count(), 0);

    // A diagonal precision gesture consumes X for tab navigation while
    // preserving Y as an ordinary terminal wheel request.
    QVERIFY(sendPixel(-120, 10'000));
    QCOMPARE(tabChange.count(), 4);
    QCOMPARE(tabChange.constLast().constFirst().toInt(), 1);
    QCOMPARE(wheel.count(), 1);
    QVERIFY(requestAt(0).rows > 0);
    QCOMPARE(requestAt(0).columns, qint64{0});

    // Disabling the frontend gesture live clears partial tab debt and restores
    // precision X to the terminal's cell-width normalization path.
    QVERIFY(sendPixel(-60));
    QCOMPARE(tabChange.count(), 4);
    LaunchOptions disabled = options;
    disabled.horizontalTabScroll = false;
    pane->applyRuntimeOptions(disabled);
    QVERIFY(sendPixel(10'000));
    QCOMPARE(tabChange.count(), 4);
    QCOMPARE(wheel.count(), 2);
    QVERIFY(requestAt(1).columns > 0);

    // Re-enabling starts with an empty accumulator. Discrete wheel buttons
    // remain terminal input regardless of the precision gesture policy.
    pane->applyRuntimeOptions(options);
    QVERIFY(sendPixel(-60));
    QCOMPARE(tabChange.count(), 4);
    QVERIFY(sendWheelEvent(*pane, QPoint{}, QPoint(120, 0)));
    QCOMPARE(tabChange.count(), 4);
    QCOMPARE(wheel.count(), 3);
    QCOMPARE(requestAt(2).rows, qint64{0});
    QCOMPARE(requestAt(2).columns, qint64{1});

    delete pane;
}

void TerminalPaneTest::forwardsTypedSelectionPointerMetadataOnce()
{
    qRegisterMetaType<TerminalSelectionPressInput>();
    qRegisterMetaType<TerminalSelectionDragInput>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    useSystemFixedFont(options);

    QQuickWindow window;
    window.resize(320, 160);
    auto *pane = new TerminalPane(options, window.contentItem(), std::nullopt,
                                  TerminalSessionStartMode::Deferred);
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QCOMPARE(pane->cursor().shape(), Qt::IBeamCursor);
    QSignalSpy presses(controller,
                       &TerminalController::beginSelectionRequested);
    QSignalSpy drags(controller, &TerminalController::updateSelectionRequested);
    QSignalSpy releases(controller, &TerminalController::endSelectionRequested);

    const qreal devicePixelRatio = window.devicePixelRatio();
    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography, devicePixelRatio);
    const QPointF pressPosition(metrics.cellWidth * 2.25,
                                metrics.cellHeight * 1.5);
    const QPointF dragPosition(metrics.cellWidth * 4.75,
                               metrics.cellHeight * 2.5);
    constexpr quint64 pressTimestampMilliseconds = 271;
    const auto sendMouse =
        [pane](QEvent::Type type, const QPointF &position,
               Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers modifiers, quint64 timestampMilliseconds) {
            QMouseEvent event(type, position, position, position, button,
                              buttons, modifiers);
            event.setTimestamp(timestampMilliseconds);
            QCoreApplication::sendEvent(pane, &event);
            QVERIFY(event.isAccepted());
        };

    sendMouse(QEvent::MouseButtonPress, pressPosition, Qt::LeftButton,
              Qt::LeftButton,
              Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier,
              pressTimestampMilliseconds);
    QCOMPARE(presses.count(), 1);
    const TerminalSelectionPressInput firstPress =
        qvariant_cast<TerminalSelectionPressInput>(
            presses.constFirst().constFirst());
    QCOMPARE(firstPress.column, 2);
    QCOMPARE(firstPress.row, 1);
    QCOMPARE(firstPress.surfaceX, pressPosition.x() * devicePixelRatio);
    QCOMPARE(firstPress.surfaceY, pressPosition.y() * devicePixelRatio);
    QCOMPARE(firstPress.timestampNanoseconds,
             pressTimestampMilliseconds * quint64{1'000'000});
    QVERIFY(firstPress.timestampValid);
    QVERIFY(firstPress.controlModifier);
    QVERIFY(firstPress.extendExistingSelection);
    QVERIFY(firstPress.rectangular);
    QCOMPARE(pane->cursor().shape(), Qt::CrossCursor);

    sendMouse(QEvent::MouseMove, dragPosition, Qt::NoButton, Qt::LeftButton,
              Qt::ControlModifier | Qt::AltModifier,
              pressTimestampMilliseconds + 1);
    QCOMPARE(drags.count(), 1);
    const TerminalSelectionDragInput drag =
        qvariant_cast<TerminalSelectionDragInput>(
            drags.constFirst().constFirst());
    QCOMPARE(drag.column, 4);
    QCOMPARE(drag.row, 2);
    QCOMPARE(drag.surfaceX, dragPosition.x() * devicePixelRatio);
    QCOMPARE(drag.surfaceY, dragPosition.y() * devicePixelRatio);
    QVERIFY(drag.rectangular);
    QCOMPARE(pane->cursor().shape(), Qt::CrossCursor);

    sendMouse(QEvent::MouseButtonRelease, dragPosition, Qt::LeftButton,
              Qt::NoButton, Qt::ControlModifier | Qt::AltModifier,
              pressTimestampMilliseconds + 2);
    QCOMPARE(releases.count(), 1);
    QCOMPARE(pane->cursor().shape(), Qt::CrossCursor);

    // Releasing either rectangle modifier must synchronously clear the
    // crosshair, without waiting for a hover move or hyperlink query.
    QKeyEvent altRelease(QEvent::KeyRelease, Qt::Key_Alt,
                         Qt::ControlModifier | Qt::AltModifier);
    QCoreApplication::sendEvent(pane, &altRelease);
    QVERIFY(altRelease.isAccepted());
    QCOMPARE(pane->cursor().shape(), Qt::IBeamCursor);
    QKeyEvent controlRelease(QEvent::KeyRelease, Qt::Key_Control,
                             Qt::ControlModifier);
    QCoreApplication::sendEvent(pane, &controlRelease);
    QVERIFY(controlRelease.isAccepted());

    // Qt sends the physical second press through mousePressEvent and follows
    // it with an informational MouseButtonDblClick event. Only the former may
    // reach libghostty, otherwise a double click would classify as a triple.
    sendMouse(QEvent::MouseButtonPress, pressPosition, Qt::LeftButton,
              Qt::LeftButton, Qt::MetaModifier,
              pressTimestampMilliseconds + 100);
    QCOMPARE(presses.count(), 2);
    const TerminalSelectionPressInput secondPress =
        qvariant_cast<TerminalSelectionPressInput>(
            presses.constLast().constFirst());
    QVERIFY(!secondPress.controlModifier);
    QVERIFY(!secondPress.extendExistingSelection);
    QVERIFY(!secondPress.rectangular);
    QCOMPARE(pane->cursor().shape(), Qt::IBeamCursor);
    sendMouse(QEvent::MouseButtonDblClick, pressPosition, Qt::LeftButton,
              Qt::LeftButton, Qt::MetaModifier,
              pressTimestampMilliseconds + 100);
    QCOMPARE(presses.count(), 2);
    QCOMPARE(drags.count(), 1);
    QCOMPARE(releases.count(), 1);

    sendMouse(QEvent::MouseButtonRelease, pressPosition, Qt::LeftButton,
              Qt::NoButton, Qt::MetaModifier, pressTimestampMilliseconds + 101);
    QCOMPARE(releases.count(), 2);

    // Zero is Qt's unavailable timestamp sentinel in synthetic/unsupported
    // input. Forward it explicitly as untimed rather than inventing an epoch.
    sendMouse(QEvent::MouseButtonPress, pressPosition, Qt::LeftButton,
              Qt::LeftButton, Qt::AltModifier, 0);
    QCOMPARE(presses.count(), 3);
    const TerminalSelectionPressInput untimedPress =
        qvariant_cast<TerminalSelectionPressInput>(
            presses.constLast().constFirst());
    QVERIFY(!untimedPress.timestampValid);
    QCOMPARE(untimedPress.timestampNanoseconds, quint64{0});
    QVERIFY(!untimedPress.rectangular);
    QCOMPARE(pane->cursor().shape(), Qt::IBeamCursor);
    sendMouse(QEvent::MouseMove, dragPosition, Qt::NoButton, Qt::LeftButton,
              Qt::AltModifier, 0);
    QCOMPARE(drags.count(), 2);
    const TerminalSelectionDragInput altOnlyDrag =
        qvariant_cast<TerminalSelectionDragInput>(
            drags.constLast().constFirst());
    QVERIFY(!altOnlyDrag.rectangular);
    sendMouse(QEvent::MouseButtonRelease, dragPosition, Qt::LeftButton,
              Qt::NoButton, Qt::AltModifier, 0);

    sendMouse(QEvent::MouseButtonPress, pressPosition, Qt::LeftButton,
              Qt::LeftButton, Qt::ControlModifier,
              std::numeric_limits<quint64>::max());
    QCOMPARE(presses.count(), 4);
    const TerminalSelectionPressInput overflowingPress =
        qvariant_cast<TerminalSelectionPressInput>(
            presses.constLast().constFirst());
    QVERIFY(!overflowingPress.timestampValid);
    QCOMPARE(overflowingPress.timestampNanoseconds, quint64{0});
    QVERIFY(!overflowingPress.rectangular);
    QCOMPARE(pane->cursor().shape(), Qt::IBeamCursor);

    delete pane;
}

void TerminalPaneTest::invalidatesInspectorRequestDuringSynchronousDispatch()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    TerminalController *const controller =
        pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QVERIFY(controller->startSession());
    QVERIFY(controller->sessionStarted());

    connect(controller, &TerminalController::terminalInspectorSnapshotRequested,
            controller, &TerminalController::beginShutdown,
            Qt::DirectConnection);
    QCOMPARE(controller->requestTerminalInspectorSnapshot(), quint64{0});
}

void TerminalPaneTest::isolatesInspectorCellPickGestures()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("stty -echo; printf '\033[?1003hcell-pick-ready'; "
                       "exec cat >/dev/null"),
    };
    options.hold = true;
    options.mouseReporting = true;
    options.horizontalTabScroll = true;
    useSystemFixedFont(options);

    TerminalPane pane(options);
    pane.setSize(QSizeF(320.0, 160.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy mouse(controller, &TerminalController::mouseRequested);
    QSignalSpy wheel(controller, &TerminalController::wheelRequested);
    QSignalSpy selectionBegin(controller,
                              &TerminalController::beginSelectionRequested);
    QSignalSpy selectionUpdate(controller,
                               &TerminalController::updateSelectionRequested);
    QSignalSpy selectionEnd(controller,
                            &TerminalController::endSelectionRequested);
    QSignalSpy rightClicks(controller,
                           &TerminalController::rightClickRequested);
    QSignalSpy hyperlinkPreparations(
        controller,
        &TerminalController::hyperlinkActivationPreparationRequested);
    QSignalSpy cellRequests(
        controller, &TerminalController::terminalInspectorCellRequested);
    QSignalSpy picked(&pane, &TerminalPane::inspectorCellPicked);
    QSignalSpy tabChanges(&pane, &TerminalPane::requestTabChange);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("cell-pick-ready")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->terminalMouseTracking(), 1000);
    QVERIFY(
        pane.controlInspector(WorkspaceFrontendActions::InspectorMode::Show));
    TerminalInspectorModel *const model = pane.inspectorModel();
    QVERIFY(model != nullptr);

    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography, 1.0);
    const QPointF firstCell(metrics.cellWidth * 0.5, metrics.cellHeight * 0.5);
    const QPointF secondCell(metrics.cellWidth * 1.5, metrics.cellHeight * 0.5);
    const auto sendMouse = [&pane](QEvent::Type type, const QPointF &position,
                                   Qt::MouseButton button,
                                   Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, position, position, button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(&pane, &event);
        QVERIFY(event.isAccepted());
    };

    model->beginCellPick();
    QVERIFY(pane.inspectorCellPicking());
    QCOMPARE(pane.cursor().shape(), Qt::CrossCursor);
    sendMouse(QEvent::MouseButtonPress, firstCell, Qt::LeftButton,
              Qt::LeftButton);
    QVERIFY(!pane.inspectorCellPicking());
    QCOMPARE(picked.count(), 1);
    QCOMPARE(cellRequests.count(), 1);
    QCOMPARE(picked.constFirst().at(0).toInt(), 0);
    QCOMPARE(picked.constFirst().at(1).toInt(), 0);
    // A chorded button and wheel event before the picked button is released
    // remain part of the consumed gesture rather than leaking unmatched input.
    sendMouse(QEvent::MouseButtonPress, firstCell, Qt::RightButton,
              Qt::LeftButton | Qt::RightButton);
    sendMouse(QEvent::MouseButtonRelease, firstCell, Qt::RightButton,
              Qt::LeftButton);
    QVERIFY(sendWheelEvent(pane, QPoint(-120, 120), QPoint{}));
    sendMouse(QEvent::MouseMove, secondCell, Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, secondCell, Qt::LeftButton,
              Qt::NoButton);
    QCOMPARE(mouse.count(), 0);
    QCOMPARE(wheel.count(), 0);
    QCOMPARE(tabChanges.count(), 0);
    QCOMPARE(selectionBegin.count(), 0);
    QCOMPARE(selectionUpdate.count(), 0);
    QCOMPARE(selectionEnd.count(), 0);
    QCOMPARE(rightClicks.count(), 0);
    QCOMPARE(hyperlinkPreparations.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(model->snapshot()
                                  .value(QStringLiteral("cell"))
                                  .toMap()
                                  .value(QStringLiteral("status"))
                                  .toString(),
                              QStringLiteral("Ready"), 1000);

    // Once the complete picked gesture is consumed, normal DEC mouse routing
    // resumes on the very next gesture.
    sendMouse(QEvent::MouseButtonPress, firstCell, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(QEvent::MouseMove, secondCell, Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, secondCell, Qt::LeftButton,
              Qt::NoButton);
    QCOMPARE(mouse.count(), 3);

    const qsizetype mouseBeforeCancel = mouse.count();
    model->beginCellPick();
    QVERIFY(sendWheelEvent(pane, QPoint{}, QPoint(0, 120)));
    QVERIFY(pane.inspectorCellPicking());
    QCOMPARE(wheel.count(), 0);
    sendMouse(QEvent::MouseButtonPress, firstCell, Qt::RightButton,
              Qt::RightButton);
    sendMouse(QEvent::MouseButtonRelease, firstCell, Qt::RightButton,
              Qt::NoButton);
    QVERIFY(!pane.inspectorCellPicking());
    QCOMPARE(mouse.count(), mouseBeforeCancel);
    QCOMPARE(rightClicks.count(), 0);

    model->beginCellPick();
    QSignalSpy keys(controller, &TerminalController::keyRequested);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &escape);
    QVERIFY(escape.isAccepted());
    QVERIFY(!pane.inspectorCellPicking());
    QCOMPARE(keys.count(), 0);
    QKeyEvent escapeRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &escapeRelease);
    QVERIFY(escapeRelease.isAccepted());
    QCOMPARE(keys.count(), 0);
}

void TerminalPaneTest::cancelsSelectionWhenMouseGrabIsRevoked()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    useSystemFixedFont(options);

    QQuickWindow window;
    window.resize(320, 160);
    auto *pane = new TerminalPane(options, window.contentItem(), std::nullopt,
                                  TerminalSessionStartMode::Deferred);
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy presses(controller,
                       &TerminalController::beginSelectionRequested);
    QSignalSpy drags(controller, &TerminalController::updateSelectionRequested);
    QSignalSpy releases(controller, &TerminalController::endSelectionRequested);
    QSignalSpy cancellations(
        controller, &TerminalController::cancelSelectionGestureRequested);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 1000);
    const QPoint pressPosition(20, 20);
    const QPoint dragPosition(40, 1);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, pressPosition);
    QCOMPARE(presses.count(), 1);
    QCOMPARE(window.mouseGrabberItem(), pane);
    QTest::mouseMove(&window, dragPosition);
    QCOMPARE(drags.count(), 1);

    pane->ungrabMouse();
    QVERIFY(window.mouseGrabberItem() == nullptr);
    QCOMPARE(cancellations.count(), 1);
    QCOMPARE(releases.count(), 0);

    // Delivery may resume under the pointer after the grab is gone, but the
    // abandoned gesture must not accept more motion or turn a later physical
    // release into a committed selection/copy boundary.
    QTest::mouseMove(&window, QPoint(60, 1));
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, QPoint(60, 1));
    QCOMPARE(drags.count(), 1);
    QCOMPARE(releases.count(), 0);
    QCOMPARE(cancellations.count(), 1);

    delete pane;
    window.close();
}

void TerminalPaneTest::togglesMouseReportingPolicyAcrossGesturesAndReloads()
{
    qRegisterMetaType<TerminalMouseInput>();
    qRegisterMetaType<TerminalWheelInput>();
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
    QSignalSpy wheel(controller, &TerminalController::wheelRequested);
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
    const auto sendWheel = [&pane](int angleDelta = 120,
                                   Qt::KeyboardModifiers modifiers =
                                       Qt::NoModifier) {
        QVERIFY(
            sendWheelEvent(pane, QPoint{}, QPoint(0, angleDelta), modifiers));
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

    // Captured fractional motion clears selection on the worker even before a
    // whole row exists and therefore emits no protocol wheel event. Reverse
    // the retained half-row so the following default-notch checks stay exact.
    controller->selectAll();
    QTRY_VERIFY_WITH_TIMEOUT(controller->selectionAvailable(), 1000);
    sendWheel(20, Qt::ShiftModifier);
    QCOMPARE(mouse.count(), 3);
    QCOMPARE(scroll.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->selectionAvailable(), 1000);
    sendWheel(-20, Qt::ShiftModifier);
    QCOMPARE(mouse.count(), 3);
    QCOMPARE(scroll.count(), 0);

    // Wheel reporting ignores Shift upstream, emits protocol button four,
    // suppresses viewport scrolling, and clears an existing selection.
    controller->selectAll();
    QTRY_VERIFY_WITH_TIMEOUT(controller->selectionAvailable(), 1000);
    sendWheel(120, Qt::ShiftModifier);
    QCOMPARE(mouse.count(), 3);
    QCOMPARE(wheel.count(), 1);
    QCOMPARE(scroll.count(), 0);
    const TerminalWheelInput upwardWheel =
        qvariant_cast<TerminalWheelInput>(wheel.constFirst().constFirst());
    QCOMPARE(upwardWheel.rows, qint64{3});
    QVERIFY(upwardWheel.modifiers & Qt::ShiftModifier);
    QVERIFY(upwardWheel.mouseReportingEnabled);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->selectionAvailable(), 1000);

    // Negative notches retain exact row-count and direction symmetry.
    sendWheel(-120, Qt::AltModifier);
    QCOMPARE(mouse.count(), 3);
    QCOMPARE(wheel.count(), 2);
    QCOMPARE(scroll.count(), 0);
    const TerminalWheelInput downwardWheel =
        qvariant_cast<TerminalWheelInput>(wheel.constLast().constFirst());
    QCOMPARE(downwardWheel.rows, qint64{-3});
    QVERIFY(downwardWheel.modifiers & Qt::AltModifier);
    QVERIFY(downwardWheel.mouseReportingEnabled);

    // Ghostty reevaluates capture for every event. Disabling after a reported
    // press suppresses its later motion/release and does not begin selection.
    sendMouse(QEvent::MouseButtonPress, start,
              Qt::LeftButton, Qt::LeftButton);
    QCOMPARE(mouse.count(), 4);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("toggle_mouse_reporting")));
    QVERIFY(!controller->mouseTracking());
    sendMouse(QEvent::MouseMove, moved,
              Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, moved,
              Qt::LeftButton, Qt::NoButton);
    QCOMPARE(mouse.count(), 4);
    QCOMPARE(selectionBegin.count(), 2);
    QCOMPARE(selectionUpdate.count(), 2);
    QCOMPARE(selectionEnd.count(), 2);

    // With raw DEC tracking still requested but policy off, the wheel returns
    // to worker-owned local viewport routing with the same signed scaling.
    sendWheel();
    sendWheel(-120);
    QCOMPARE(mouse.count(), 4);
    QCOMPARE(wheel.count(), 4);
    QCOMPARE(scroll.count(), 0);
    const TerminalWheelInput localUp =
        qvariant_cast<TerminalWheelInput>(wheel.at(2).constFirst());
    QCOMPARE(localUp.rows, qint64{3});
    QVERIFY(!localUp.mouseReportingEnabled);
    const TerminalWheelInput localDown =
        qvariant_cast<TerminalWheelInput>(wheel.at(3).constFirst());
    QCOMPARE(localDown.rows, qint64{-3});
    QVERIFY(!localDown.mouseReportingEnabled);

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
    QCOMPARE(mouse.count(), 6);
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
    QCOMPARE(mouse.count(), 6);
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
    QCOMPARE(mouse.count(), 9);
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

    // Captured routing uses repeated protocol events instead of the local
    // O(1) viewport request. A two-notch extreme is bounded to 10,000 reports
    // in this GUI dispatch, while the retained second-notch debt cancels
    // against an opposite notch without producing another report.
    LaunchOptions extreme = enabled;
    extreme.mouseScrollMultiplier.discrete = 10'000.0;
    pane.applyRuntimeOptions(extreme);
    const qsizetype beforeExtreme = wheel.count();
    sendWheel(240);
    QCOMPARE(wheel.count() - beforeExtreme, 1);
    const TerminalWheelInput extremeWheel =
        qvariant_cast<TerminalWheelInput>(wheel.at(beforeExtreme).constFirst());
    QCOMPARE(extremeWheel.rows, qint64{10'000});
    QVERIFY(extremeWheel.mouseReportingEnabled);
    sendWheel(-120);
    QCOMPARE(wheel.count() - beforeExtreme, 1);
}

void TerminalPaneTest::appliesMouseShiftCaptureAcrossPointerRoutes()
{
    qRegisterMetaType<TerminalMouseInput>();
    qRegisterMetaType<TerminalWheelInput>();
    qRegisterMetaType<TerminalRightClickInput>();
    qRegisterMetaType<TerminalSelectionPressInput>();

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("stty -echo; printf '\\033[?1003hshift-capture-ready'; "
                       "exec cat >/dev/null"),
    };
    options.hold = true;
    options.mouseReporting = true;
    options.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;

    TerminalPane pane(options);
    pane.setSize(QSizeF(320.0, 160.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy mouse(controller, &TerminalController::mouseRequested);
    QSignalSpy wheel(controller, &TerminalController::wheelRequested);
    QSignalSpy selectionBegin(controller,
                              &TerminalController::beginSelectionRequested);
    QSignalSpy selectionUpdate(controller,
                               &TerminalController::updateSelectionRequested);
    QSignalSpy selectionEnd(controller,
                            &TerminalController::endSelectionRequested);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);
    QSignalSpy rightClicks(controller,
                           &TerminalController::rightClickRequested);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("shift-capture-ready")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->terminalMouseTracking(), 1000);
    QVERIFY(controller->mouseTracking());

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    clipboard->setText(QStringLiteral("shift-capture-paste"),
                       QClipboard::Clipboard);
    if (clipboard->supportsSelection()) {
        clipboard->setText(QStringLiteral("shift-capture-paste"),
                           QClipboard::Selection);
    }

    const QPointF start(8.0, 8.0);
    const QPointF moved(24.0, 8.0);
    const auto sendMouse = [&](QEvent::Type type, const QPointF &position,
                               Qt::MouseButton button, Qt::MouseButtons buttons,
                               Qt::KeyboardModifiers modifiers) {
        QMouseEvent event(type, position, position, position, button, buttons,
                          modifiers);
        QCoreApplication::sendEvent(&pane, &event);
        QVERIFY(event.isAccepted());
    };
    const auto sendShiftGesture = [&] {
        sendMouse(QEvent::MouseButtonPress, start, Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(QEvent::MouseMove, moved, Qt::NoButton, Qt::LeftButton,
                  Qt::ShiftModifier);
        sendMouse(QEvent::MouseButtonRelease, moved, Qt::LeftButton,
                  Qt::NoButton, Qt::ShiftModifier);
    };
    const auto sendHover = [&](Qt::KeyboardModifiers modifiers) {
        QHoverEvent event(QEvent::HoverMove, start, moved, moved, modifiers);
        QCoreApplication::sendEvent(&pane, &event);
        QVERIFY(event.isAccepted());
    };

    struct PolicyCase {
        MouseShiftCapture mode;
        bool captures;
    };
    constexpr std::array cases{
        PolicyCase{MouseShiftCapture::False, false},
        PolicyCase{MouseShiftCapture::True, true},
        PolicyCase{MouseShiftCapture::Always, true},
        PolicyCase{MouseShiftCapture::Never, false},
    };
    for (const PolicyCase policy : cases) {
        LaunchOptions reloaded = options;
        reloaded.mouseShiftCapture = policy.mode;
        pane.applyRuntimeOptions(reloaded);

        const qsizetype mouseBeforeGesture = mouse.size();
        const qsizetype beginBefore = selectionBegin.size();
        const qsizetype updateBefore = selectionUpdate.size();
        const qsizetype endBefore = selectionEnd.size();
        sendShiftGesture();
        if (policy.captures) {
            QCOMPARE(mouse.size(), mouseBeforeGesture + 3);
            QCOMPARE(selectionBegin.size(), beginBefore);
            QCOMPARE(selectionUpdate.size(), updateBefore);
            QCOMPARE(selectionEnd.size(), endBefore);
            for (qsizetype index = mouseBeforeGesture;
                 index < mouseBeforeGesture + 3; ++index) {
                const TerminalMouseInput input =
                    qvariant_cast<TerminalMouseInput>(
                        mouse.at(index).constFirst());
                QVERIFY(input.modifiers & Qt::ShiftModifier);
            }
        } else {
            QCOMPARE(mouse.size(), mouseBeforeGesture);
            QCOMPARE(selectionBegin.size(), beginBefore + 1);
            QCOMPARE(selectionUpdate.size(), updateBefore + 1);
            QCOMPARE(selectionEnd.size(), endBefore + 1);
            const TerminalSelectionPressInput input =
                qvariant_cast<TerminalSelectionPressInput>(
                    selectionBegin.constLast().constFirst());
            QVERIFY(input.extendExistingSelection);
        }

        const qsizetype mouseBeforeMiddle = mouse.size();
        const qsizetype pasteBefore = pasted.size();
        sendMouse(QEvent::MouseButtonPress, start, Qt::MiddleButton,
                  Qt::MiddleButton, Qt::ShiftModifier);
        QCOMPARE(mouse.size(), mouseBeforeMiddle + (policy.captures ? 1 : 0));
        QCOMPARE(pasted.size(), pasteBefore + (policy.captures ? 0 : 1));

        const qsizetype mouseBeforeRight = mouse.size();
        const qsizetype rightBefore = rightClicks.size();
        sendMouse(QEvent::MouseButtonPress, start, Qt::RightButton,
                  Qt::RightButton, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(mouse.size(), mouseBeforeRight + (policy.captures ? 1 : 0));
        QCOMPARE(rightClicks.size(), rightBefore + (policy.captures ? 0 : 1));
        if (!policy.captures) {
            const TerminalRightClickInput input =
                qvariant_cast<TerminalRightClickInput>(
                    rightClicks.constLast().constFirst());
            QVERIFY(input.shiftBypassedMouseCapture);
        }

        // Upstream deliberately excludes both DEC reporting routes from
        // mouse-shift-capture: no-button hover remains a motion report, and
        // a wheel request retains Shift for worker-owned DEC routing. Local
        // hyperlink eligibility is covered separately below.
        const qsizetype mouseBeforeHover = mouse.size();
        sendHover(Qt::ShiftModifier);
        QCOMPARE(mouse.size(), mouseBeforeHover + 1);
        const TerminalMouseInput hover =
            qvariant_cast<TerminalMouseInput>(mouse.constLast().constFirst());
        QCOMPARE(hover.action, TerminalMouseInput::Motion);
        QVERIFY(hover.modifiers & Qt::ShiftModifier);

        // Pointer shape follows Ghostty's raw DEC state rather than the
        // frontend reporting toggle or shift-capture policy. Ctrl+Alt alone
        // remains the captured arrow; adding Shift exposes the crosshair.
        sendHover(Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(pane.cursor().shape(), Qt::ArrowCursor);
        sendHover(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(pane.cursor().shape(), Qt::CrossCursor);
        sendHover(Qt::ShiftModifier);
        QCOMPARE(pane.cursor().shape(), Qt::IBeamCursor);

        const qsizetype mouseBeforeWheel = mouse.size();
        const qsizetype wheelBefore = wheel.size();
        QVERIFY(
            sendWheelEvent(pane, QPoint{}, QPoint(0, 120), Qt::ShiftModifier));
        QCOMPARE(mouse.size(), mouseBeforeWheel);
        QCOMPARE(wheel.size(), wheelBefore + 1);
        const TerminalWheelInput input =
            qvariant_cast<TerminalWheelInput>(wheel.constLast().constFirst());
        QCOMPARE(input.rows, qint64{3});
        QVERIFY(input.modifiers & Qt::ShiftModifier);
        QVERIFY(input.mouseReportingEnabled);
    }

    // The independent frontend reporting gate makes every policy local, but
    // Ghostty still consults mouse-shift-capture before deciding whether that
    // local Shift press may extend an existing selection.
    LaunchOptions disabled = options;
    disabled.mouseReporting = false;
    for (const PolicyCase policy : cases) {
        disabled.mouseShiftCapture = policy.mode;
        pane.applyRuntimeOptions(disabled);
        QVERIFY(controller->terminalMouseTracking());
        QVERIFY(!controller->mouseTracking());
        const qsizetype mouseBeforeDisabled = mouse.size();
        const qsizetype beginBeforeDisabled = selectionBegin.size();
        sendShiftGesture();
        QCOMPARE(mouse.size(), mouseBeforeDisabled);
        QCOMPARE(selectionBegin.size(), beginBeforeDisabled + 1);
        const TerminalSelectionPressInput input =
            qvariant_cast<TerminalSelectionPressInput>(
                selectionBegin.constLast().constFirst());
        QCOMPARE(input.extendExistingSelection, !policy.captures);
    }

    // Raw DEC state remains authoritative for deciding whether a local right
    // click used the Shift escape even while reporting is disabled.
    disabled.mouseShiftCapture = MouseShiftCapture::Always;
    pane.applyRuntimeOptions(disabled);
    sendHover(Qt::ControlModifier | Qt::AltModifier);
    QCOMPARE(pane.cursor().shape(), Qt::ArrowCursor);
    sendHover(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
    QCOMPARE(pane.cursor().shape(), Qt::CrossCursor);
    sendHover(Qt::NoModifier);
    QCOMPARE(pane.cursor().shape(), Qt::ArrowCursor);
    const qsizetype rightBeforeAlways = rightClicks.size();
    sendMouse(QEvent::MouseButtonPress, start, Qt::RightButton, Qt::RightButton,
              Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(rightClicks.size(), rightBeforeAlways + 1);
    QVERIFY(!qvariant_cast<TerminalRightClickInput>(
                 rightClicks.constLast().constFirst())
                 .shiftBypassedMouseCapture);

    disabled.mouseShiftCapture = MouseShiftCapture::Never;
    pane.applyRuntimeOptions(disabled);
    const qsizetype rightBeforeNever = rightClicks.size();
    sendMouse(QEvent::MouseButtonPress, start, Qt::RightButton, Qt::RightButton,
              Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(rightClicks.size(), rightBeforeNever + 1);
    QVERIFY(qvariant_cast<TerminalRightClickInput>(
                rightClicks.constLast().constFirst())
                .shiftBypassedMouseCapture);

    clipboard->clear(QClipboard::Clipboard);
    if (clipboard->supportsSelection()) {
        clipboard->clear(QClipboard::Selection);
    }
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

void TerminalPaneTest::convertsTerminalDropContent()
{
    QCOMPARE(escapeTerminalDropPath(QStringLiteral("a\\b\"c'd$e`f*g?h i|j(k)")),
             QStringLiteral("a\\\\b\\\"c\\'d\\$e\\`f\\*g\\?h\\ i\\|j\\(k\\)"));

    QMimeData urls;
    urls.setText(QStringLiteral("text fallback must not be used"));
    urls.setUrls({
        QUrl(QStringLiteral("https://example.com/remote")),
        QUrl::fromLocalFile(QStringLiteral("/tmp/a b$'")),
        QUrl::fromLocalFile(QStringLiteral("/tmp/(x)|y?*")),
    });
    const TerminalDropContent urlContent = terminalDropContent(urls);
    QVERIFY(urlContent.recognized);
    QCOMPARE(urlContent.text,
             QStringLiteral("/tmp/a\\ b\\$\\'\n/tmp/\\(x\\)\\|y\\?\\*\n"));

    QMimeData remoteOnly;
    remoteOnly.setText(QStringLiteral("must remain hidden"));
    remoteOnly.setUrls({
        QUrl(QStringLiteral("https://example.com/one")),
        QUrl(QStringLiteral("sftp://example.com/two")),
    });
    const TerminalDropContent remoteContent = terminalDropContent(remoteOnly);
    QVERIFY(remoteContent.recognized);
    QVERIFY(remoteContent.text.isEmpty());

    QMimeData text;
    text.setText(QStringLiteral("λ plain text\nwithout an added newline"));
    const TerminalDropContent textContent = terminalDropContent(text);
    QVERIFY(textContent.recognized);
    QCOMPARE(textContent.text,
             QStringLiteral("λ plain text\nwithout an added newline"));

    QMimeData binary;
    binary.setData(QStringLiteral("application/octet-stream"),
                   QByteArrayLiteral("not text"));
    const TerminalDropContent binaryContent = terminalDropContent(binary);
    QVERIFY(!binaryContent.recognized);
    QVERIFY(binaryContent.text.isEmpty());
}

void TerminalPaneTest::routesTerminalDropsThroughPasteController()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.clipboardPaste.protection = false;
    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    QVERIFY(pane.flags().testFlag(QQuickItem::ItemAcceptsDrops));

    auto *const controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);

    QMimeData binary;
    binary.setData(QStringLiteral("application/octet-stream"),
                   QByteArrayLiteral("ignored"));
    QDragEnterEvent binaryEnter(QPoint(2, 3), Qt::CopyAction | Qt::MoveAction,
                                &binary, Qt::LeftButton, Qt::NoModifier);
    binaryEnter.ignore();
    QCoreApplication::sendEvent(&pane, &binaryEnter);
    QVERIFY(!binaryEnter.isAccepted());

    QMimeData paths;
    paths.setText(QStringLiteral("fallback"));
    paths.setUrls({
        QUrl::fromLocalFile(QStringLiteral("/tmp/first file")),
        QUrl(QStringLiteral("https://example.com/skip")),
        QUrl::fromLocalFile(QStringLiteral("/tmp/second")),
    });
    QDragEnterEvent pathEnter(QPoint(2, 3), Qt::CopyAction | Qt::MoveAction,
                              &paths, Qt::LeftButton, Qt::ControlModifier);
    pathEnter.ignore();
    QCoreApplication::sendEvent(&pane, &pathEnter);
    QVERIFY(pathEnter.isAccepted());
    QCOMPARE(pathEnter.dropAction(), Qt::CopyAction);

    QDragMoveEvent pathMove(QPoint(4, 5), Qt::CopyAction | Qt::MoveAction,
                            &paths, Qt::LeftButton, Qt::ControlModifier);
    pathMove.ignore();
    QCoreApplication::sendEvent(&pane, &pathMove);
    QVERIFY(pathMove.isAccepted());
    QCOMPARE(pathMove.dropAction(), Qt::CopyAction);

    QDropEvent pathDrop(QPointF(4, 5), Qt::CopyAction | Qt::MoveAction, &paths,
                        Qt::LeftButton, Qt::ControlModifier);
    pathDrop.ignore();
    QCoreApplication::sendEvent(&pane, &pathDrop);
    QVERIFY(pathDrop.isAccepted());
    QCOMPARE(pathDrop.dropAction(), Qt::CopyAction);
    QCOMPARE(pasted.count(), 1);
    QCOMPARE(pasted.constFirst().constFirst().toString(),
             QStringLiteral("/tmp/first\\ file\n/tmp/second\n"));

    QMimeData remoteOnly;
    remoteOnly.setText(QStringLiteral("must not be pasted"));
    remoteOnly.setUrls({QUrl(QStringLiteral("https://example.com/file"))});
    QDropEvent remoteDrop(QPointF(1, 1), Qt::CopyAction, &remoteOnly,
                          Qt::NoButton, Qt::NoModifier);
    remoteDrop.ignore();
    QCoreApplication::sendEvent(&pane, &remoteDrop);
    QVERIFY(remoteDrop.isAccepted());
    QCOMPARE(remoteDrop.dropAction(), Qt::CopyAction);
    QCOMPARE(pasted.count(), 1);

    QMimeData text;
    text.setText(QStringLiteral("λ text\nexact"));
    QDropEvent textDrop(QPointF(1, 1), Qt::CopyAction, &text, Qt::NoButton,
                        Qt::NoModifier);
    textDrop.ignore();
    QCoreApplication::sendEvent(&pane, &textDrop);
    QVERIFY(textDrop.isAccepted());
    QCOMPARE(pasted.count(), 2);
    QCOMPARE(pasted.constLast().constFirst().toString(),
             QStringLiteral("λ text\nexact"));

    QDropEvent moveOnlyDrop(QPointF(1, 1), Qt::MoveAction, &text, Qt::NoButton,
                            Qt::NoModifier);
    moveOnlyDrop.ignore();
    QCoreApplication::sendEvent(&pane, &moveOnlyDrop);
    QVERIFY(!moveOnlyDrop.isAccepted());
    QCOMPARE(pasted.count(), 2);
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
    QMimeData droppedText;
    droppedText.setText(rejected);
    QDropEvent protectedDrop(QPointF(1, 1), Qt::CopyAction, &droppedText,
                             Qt::NoButton, Qt::NoModifier);
    protectedDrop.ignore();
    QCoreApplication::sendEvent(&pane, &protectedDrop);
    QVERIFY(protectedDrop.isAccepted());
    QCOMPARE(protectedDrop.dropAction(), Qt::CopyAction);
    QCOMPARE(pasted.count(), 1);
    QCOMPARE(pasted.constFirst().constFirst().toString(), rejected);
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

void TerminalPaneTest::reconcilesActivityAfterKamRejectsEnter()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.ordinaryCommand = TerminalCommand::shell(
        QByteArrayLiteral("printf '\\033[2hkam-activity-ready'; "
                          "while IFS= read -r line; do :; done"),
        true);
    options.hold = true;
    options.vtKamAllowed = true;
    options.mouseHideWhileTyping = true;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy activity(controller, &TerminalController::activeProcessChanged);
    QSignalSpy errors(controller, &TerminalController::errorOccurred);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(updates, QStringLiteral("kam-activity-ready")), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->keyboardActionMode(), 3000);
    QVERIFY(controller->keyboardInputSuppressed());
    QTRY_VERIFY_WITH_TIMEOUT(!controller->activeProcess(), 3000);
    activity.clear();

    const Qt::CursorShape cursorBeforeBlockedInput = pane.cursor().shape();
    QKeyEvent blockedText(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier,
                          QStringLiteral("x"));
    QCoreApplication::sendEvent(&pane, &blockedText);
    QCOMPARE(pane.cursor().shape(), cursorBeforeBlockedInput);

    QInputMethodEvent blockedIme;
    blockedIme.setCommitString(QStringLiteral("y"));
    QCoreApplication::sendEvent(&pane, &blockedIme);
    QCOMPARE(pane.cursor().shape(), cursorBeforeBlockedInput);

    TerminalKeyInput blockedEnter;
    blockedEnter.key = Qt::Key_Return;
    blockedEnter.text = QStringLiteral("\n");
    blockedEnter.pressed = true;
    controller->sendKey(blockedEnter);

    // The GUI's conservative transition is immediate. The worker then
    // rejects the key under KAM and publishes its unchanged idle state so the
    // close-confirmation policy cannot remain stuck.
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(activity, true), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(spyContainsBool(activity, false), 1000);
    QVERIFY(!controller->activeProcess());

    LaunchOptions disabled = options;
    disabled.vtKamAllowed = false;
    pane.applyRuntimeOptions(disabled);
    QVERIFY(!controller->keyboardInputSuppressed());
    QKeyEvent permittedText(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier,
                            QStringLiteral("z"));
    QCoreApplication::sendEvent(&pane, &permittedText);
    QCOMPARE(pane.cursor().shape(), Qt::BlankCursor);

    QVERIFY2(errors.isEmpty(),
             errors.isEmpty()
                 ? ""
                 : qPrintable(errors.constFirst().constFirst().toString()));
}

void TerminalPaneTest::
    hidesPointerOnlyForTerminalTypingAndRestoresOnInteraction()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    GhosttyKeybindConfig keybinds;
    keybinds.root = {
        generationTestBinding({generationTestKey('n', GhosttyKeybindCtrl)},
                              QStringLiteral("new_tab")),
        generationTestBinding(
            {
                generationTestKey('x', GhosttyKeybindCtrl),
                generationTestKey('y'),
            },
            QStringLiteral("new_tab")),
    };
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(keybinds));

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    pane.setSize(QSizeF(400.0, 300.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy newTabs(&pane, &TerminalPane::requestNewTab);
    QSignalSpy sequenceResolutions(
        controller, &TerminalController::sequenceResolutionRequested);

    const auto pressText = [&pane](int key, const QString &text,
                                   Qt::KeyboardModifiers modifiers =
                                       Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
        QCoreApplication::sendEvent(&pane, &event);
        QVERIFY(event.isAccepted());
    };
    const auto expectVisible = [&pane] {
        QVERIFY(pane.cursor().shape() != Qt::BlankCursor);
    };
    const auto expectHidden = [&pane] {
        QCOMPARE(pane.cursor().shape(), Qt::BlankCursor);
    };

    // The Ghostty default is disabled, so terminal-bound text remains
    // visible until the live option is enabled.
    QCOMPARE(options.mouseHideWhileTyping, false);
    pressText(Qt::Key_A, QStringLiteral("a"));
    QCOMPARE(forwarded.count(), 1);
    expectVisible();

    LaunchOptions enabled = options;
    enabled.mouseHideWhileTyping = true;
    pane.applyRuntimeOptions(enabled);

    // Modifier-only, non-text, release, and consumed configured input are
    // not terminal text entry and must leave the pointer visible.
    QKeyEvent controlPress(QEvent::KeyPress, Qt::Key_Control,
                           Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &controlPress);
    QVERIFY(controlPress.isAccepted());
    expectVisible();

    const int beforeNonText = forwarded.count();
    QKeyEvent leftPress(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &leftPress);
    QVERIFY(leftPress.isAccepted());
    QCOMPARE(forwarded.count(), beforeNonText + 1);
    expectVisible();

    const int beforeRelease = forwarded.count();
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier,
                      QStringLiteral("a"));
    QCoreApplication::sendEvent(&pane, &release);
    QVERIFY(release.isAccepted());
    QCOMPARE(forwarded.count(), beforeRelease + 1);
    expectVisible();

    const int beforeRepeat = forwarded.count();
    QKeyEvent repeat(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier,
                     QStringLiteral("r"), true, 2);
    QCoreApplication::sendEvent(&pane, &repeat);
    QVERIFY(repeat.isAccepted());
    QCOMPARE(forwarded.count(), beforeRepeat + 1);
    expectVisible();

    const int beforeConsumed = forwarded.count();
    QKeyEvent consumed(QEvent::KeyPress, Qt::Key_N, Qt::ControlModifier,
                       QStringLiteral("n"));
    QCoreApplication::sendEvent(&pane, &consumed);
    QVERIFY(consumed.isAccepted());
    QCOMPARE(newTabs.count(), 1);
    QCOMPARE(forwarded.count(), beforeConsumed);
    expectVisible();

    // A leader is delayed and does not hide by itself. If its continuation
    // is invalid, the atomic FlushAndSendCurrent path becomes terminal text
    // input and hides without also emitting the ordinary key signal.
    QKeyEvent leader(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier,
                     QString(QChar(0x18)));
    QCoreApplication::sendEvent(&pane, &leader);
    expectVisible();
    const int beforeInvalid = forwarded.count();
    QKeyEvent invalid(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier,
                      QStringLiteral("z"));
    QCoreApplication::sendEvent(&pane, &invalid);
    QCOMPARE(forwarded.count(), beforeInvalid);
    QCOMPARE(sequenceResolutions.count(), 1);
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 sequenceResolutions.constLast().at(1)),
             TerminalSequenceResolution::FlushAndSendCurrent);
    expectHidden();

    const QPointF firstPosition(8.0, 8.0);
    const QPointF secondPosition(9.0, 8.0);
    const QPointF thirdPosition(10.0, 8.0);
    QHoverEvent initialHover(QEvent::HoverMove, firstPosition, firstPosition,
                             firstPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &initialHover);
    expectVisible();

    // A pass-through key press with text is the only keyboard path that
    // hides the pointer.
    const int beforeText = forwarded.count();
    pressText(Qt::Key_B, QStringLiteral("b"));
    QCOMPARE(forwarded.count(), beforeText + 1);
    expectHidden();

    // Reloading while the policy remains enabled must preserve the newer
    // typing state, even though the immutable keybinding generation changes.
    pane.applyRuntimeOptions(enabled);
    expectHidden();

    // Compositors may deliver a synthetic hover after the cursor changes.
    // A same-position event must not instantly undo typing concealment.
    QHoverEvent phantomHover(QEvent::HoverMove, firstPosition, firstPosition,
                             firstPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &phantomHover);
    QVERIFY(phantomHover.isAccepted());
    expectHidden();

    // Rejected sub-physical-pixel events do not move the accepted baseline,
    // so real high-resolution motion accumulates until it reaches one device
    // pixel.
    const QPointF firstSubPixelPosition = firstPosition + QPointF(0.4, 0.0);
    QHoverEvent firstSubPixelHover(QEvent::HoverMove, firstSubPixelPosition,
                                   firstSubPixelPosition, firstPosition,
                                   Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &firstSubPixelHover);
    QVERIFY(firstSubPixelHover.isAccepted());
    expectHidden();
    const QPointF secondSubPixelPosition = firstPosition + QPointF(0.8, 0.0);
    QHoverEvent secondSubPixelHover(QEvent::HoverMove, secondSubPixelPosition,
                                    secondSubPixelPosition,
                                    firstSubPixelPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &secondSubPixelHover);
    QVERIFY(secondSubPixelHover.isAccepted());
    expectHidden();

    // At DPR 1, the accumulated one logical pixel is one physical pixel and
    // constitutes actual pointer movement.
    QHoverEvent hover(QEvent::HoverMove, secondPosition, secondPosition,
                      secondSubPixelPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &hover);
    QVERIFY(hover.isAccepted());
    expectVisible();

    QInputMethodEvent preedit(QStringLiteral("compose"), {});
    QCoreApplication::sendEvent(&pane, &preedit);
    QVERIFY(preedit.isAccepted());
    expectVisible();

    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("é"));
    QCoreApplication::sendEvent(&pane, &commit);
    QVERIFY(commit.isAccepted());
    expectHidden();

    QHoverEvent imeRestore(QEvent::HoverMove, thirdPosition, thirdPosition,
                           secondPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &imeRestore);
    QVERIFY(imeRestore.isAccepted());
    expectVisible();

    pressText(Qt::Key_C, QStringLiteral("c"));
    expectHidden();
    QMouseEvent button(QEvent::MouseButtonPress, firstPosition, firstPosition,
                       firstPosition, Qt::RightButton, Qt::RightButton,
                       Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &button);
    QVERIFY(button.isAccepted());
    expectVisible();

    pressText(Qt::Key_H, QStringLiteral("h"));
    expectHidden();
    QMouseEvent buttonRelease(QEvent::MouseButtonRelease, firstPosition,
                              firstPosition, firstPosition, Qt::RightButton,
                              Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &buttonRelease);
    QVERIFY(buttonRelease.isAccepted());
    expectVisible();

    pressText(Qt::Key_I, QStringLiteral("i"));
    expectHidden();
    QHoverEvent hoverLeave(QEvent::HoverLeave, QPointF(), QPointF(),
                           thirdPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &hoverLeave);
    QVERIFY(hoverLeave.isAccepted());
    expectVisible();

    pressText(Qt::Key_J, QStringLiteral("j"));
    expectHidden();
    const QPointF enterPosition = thirdPosition + QPointF(1.0, 0.0);
    QHoverEvent hoverEnter(QEvent::HoverEnter, enterPosition, enterPosition,
                           thirdPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &hoverEnter);
    QVERIFY(hoverEnter.isAccepted());
    expectVisible();

    pressText(Qt::Key_D, QStringLiteral("d"));
    expectHidden();
    QVERIFY(sendWheelEvent(pane, QPoint(), QPoint(0, 120)));
    expectVisible();

    pressText(Qt::Key_E, QStringLiteral("e"));
    expectHidden();
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(&pane, &focusOut);
    expectVisible();

    pressText(Qt::Key_F, QStringLiteral("f"));
    expectHidden();
    QFocusEvent focusIn(QEvent::FocusIn, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(&pane, &focusIn);
    expectVisible();

    pressText(Qt::Key_G, QStringLiteral("g"));
    expectHidden();
    QHoverEvent postFocusPhantom(QEvent::HoverMove, enterPosition,
                                 enterPosition, enterPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &postFocusPhantom);
    QVERIFY(postFocusPhantom.isAccepted());
    expectHidden();
    pane.applyRuntimeOptions(options);
    expectVisible();
}

void TerminalPaneTest::focusesPaneAfterPhysicalPointerMotionAndReload()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    QQuickWindow window;
    window.resize(400, 300);
    auto *pane = new TerminalPane(options, window.contentItem(), std::nullopt,
                                  TerminalSessionStartMode::Deferred);
    pane->setSize(window.size());
    auto *focusSink = new QQuickItem(window.contentItem());
    focusSink->setSize(QSizeF(1.0, 1.0));
    focusSink->setPosition(
        QPointF(window.width() - 1.0, window.height() - 1.0));
    focusSink->setFocusPolicy(Qt::StrongFocus);

    window.show();
    window.requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 3000);
    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);
    QSignalSpy activations(pane, &TerminalPane::activated);

    const qreal physicalPixel = 1.0 / window.devicePixelRatio();
    const QPointF initialPosition(8.0, 8.0);
    QPointF previousPosition = initialPosition;
    const auto hoverAt = [pane, &previousPosition](const QPointF &position) {
        QHoverEvent event(QEvent::HoverMove, position, position,
                          previousPosition, Qt::NoModifier);
        QCoreApplication::sendEvent(pane, &event);
        QVERIFY(event.isAccepted());
        previousPosition = position;
    };

    // The Ghostty default leaves keyboard focus unchanged while still
    // establishing the pane's accepted pointer baseline.
    QCOMPARE(options.focusFollowsMouse, false);
    hoverAt(initialPosition);
    QCOMPARE(window.activeFocusItem(), focusSink);

    LaunchOptions enabled = options;
    enabled.focusFollowsMouse = true;
    pane->applyRuntimeOptions(enabled);

    // Reload alone, same-position compositor events, and accumulated motion
    // below one device pixel must not steal keyboard focus.
    hoverAt(initialPosition);
    QCOMPARE(window.activeFocusItem(), focusSink);
    hoverAt(initialPosition + QPointF(0.4 * physicalPixel, 0.0));
    QCOMPARE(window.activeFocusItem(), focusSink);
    hoverAt(initialPosition + QPointF(0.8 * physicalPixel, 0.0));
    QCOMPARE(window.activeFocusItem(), focusSink);

    // The accepted baseline remains at the original point, so the final
    // fraction reaches exactly one physical pixel and focuses the pane.
    QPointF acceptedPosition = initialPosition + QPointF(physicalPixel, 0.0);
    hoverAt(acceptedPosition);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), pane, 1000);
    QCOMPARE(activations.count(), 1);

    // Further accepted movement within the already-focused pane must not
    // republish activation.
    acceptedPosition += QPointF(physicalPixel, 0.0);
    hoverAt(acceptedPosition);
    QCOMPARE(window.activeFocusItem(), pane);
    QCOMPARE(activations.count(), 1);

    // Disabling applies live. Physical pointer motion still updates the
    // accepted baseline, but no longer changes focus.
    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);
    pane->applyRuntimeOptions(options);
    acceptedPosition += QPointF(physicalPixel, 0.0);
    hoverAt(acceptedPosition);
    QCOMPARE(window.activeFocusItem(), focusSink);

    pane->applyRuntimeOptions(enabled);
    hoverAt(acceptedPosition);
    QCOMPARE(window.activeFocusItem(), focusSink);
    acceptedPosition += QPointF(physicalPixel, 0.0);
    hoverAt(acceptedPosition);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), pane, 1000);

    // Pointer motion in an inactive window must never raise the window or
    // publish pane activation.
    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);
    QQuickWindow secondWindow;
    secondWindow.resize(window.size());
    secondWindow.show();
    secondWindow.requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(secondWindow.isExposed(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(secondWindow.isActive(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isActive(), 1000);

    const int activationsBeforeInactiveMotion = activations.count();
    acceptedPosition += QPointF(physicalPixel, 0.0);
    hoverAt(acceptedPosition);
    QVERIFY(!window.isActive());
    QVERIFY(!pane->hasActiveFocus());
    QCOMPARE(activations.count(), activationsBeforeInactiveMotion);

    secondWindow.close();
    window.requestActivate();
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 1000);
    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);

    // Window systems may synthesize pointer motion while activation changes.
    // Establish a deterministic baseline with the option disabled, then
    // verify that re-enabling alone and a same-position hover remain inert.
    pane->applyRuntimeOptions(options);
    acceptedPosition = QPointF(200.0, 100.0);
    hoverAt(acceptedPosition);
    QCOMPARE(window.activeFocusItem(), focusSink);
    pane->applyRuntimeOptions(enabled);
    const int activationsBeforeSamePosition = activations.count();
    hoverAt(acceptedPosition);
    QCOMPARE(window.activeFocusItem(), focusSink);
    QCOMPARE(activations.count(), activationsBeforeSamePosition);
    acceptedPosition += QPointF(physicalPixel, 0.0);
    hoverAt(acceptedPosition);
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), pane, 1000);
    QCOMPARE(activations.count(), activationsBeforeSamePosition + 1);

    // Button-drag motion uses the same focus path. Its focus-in activation
    // observers may synchronously remove the pane, so no subsequent mouse
    // dispatch may dereference that pane.
    focusSink->forceActiveFocus();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeFocusItem(), focusSink, 1000);
    auto *doomedPane =
        new TerminalPane(options, window.contentItem(), std::nullopt,
                         TerminalSessionStartMode::Deferred);
    doomedPane->setSize(window.size());
    const QPointF doomedBaseline(20.0, 20.0);
    QHoverEvent doomedInitialHover(QEvent::HoverMove, doomedBaseline,
                                   doomedBaseline, doomedBaseline,
                                   Qt::NoModifier);
    QCoreApplication::sendEvent(doomedPane, &doomedInitialHover);
    doomedPane->applyRuntimeOptions(enabled);
    const QPointer<TerminalPane> doomedGuard(doomedPane);
    QObject::connect(doomedPane, &TerminalPane::activated, &window,
                     [doomedPane](TerminalPane *) { delete doomedPane; });

    const QPointF draggedPosition =
        doomedBaseline + QPointF(physicalPixel, 0.0);
    QMouseEvent drag(QEvent::MouseMove, draggedPosition, draggedPosition,
                     draggedPosition, Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(doomedPane, &drag);
    QVERIFY(drag.isAccepted());
    QVERIFY(doomedGuard.isNull());

    pane->setParentItem(nullptr);
    delete pane;
    window.close();
}

void TerminalPaneTest::asyncFallbackDoesNotHideAfterPointerActivity()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.mouseHideWhileTyping = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("performable:alt+x=copy_to_clipboard:plain"),
    });

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    pane.setSize(QSizeF(400.0, 300.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    // Isolate pane-side completion ordering: the real worker path is covered
    // elsewhere, while this test controls exactly when the unavailable
    // result reaches the suspended performable binding.
    const QMetaMethod copySignal =
        QMetaMethod::fromSignal(&TerminalController::copyActionRequested);
    QVERIFY(
        QObject::disconnect(controller, copySignal, nullptr, QMetaMethod{}));
    QSignalSpy copies(controller, &TerminalController::copyActionRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    const QPointF firstPosition(8.0, 8.0);
    const QPointF secondPosition(9.0, 8.0);
    const QPointF thirdPosition(10.0, 8.0);
    QHoverEvent initialHover(QEvent::HoverMove, firstPosition, firstPosition,
                             firstPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &initialHover);

    const auto pressPerformable = [&pane] {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_X, Qt::AltModifier,
                        QStringLiteral("x"));
        QCoreApplication::sendEvent(&pane, &event);
        QVERIFY(event.isAccepted());
    };
    const auto completeUnavailable = [controller](quint64 requestId) {
        Q_EMIT controller->terminalActionReady({
            .requestId = requestId,
            .outcome = TerminalActionOutcome::Unavailable,
            .effect = TerminalActionEffect::None,
            .performed = false,
        });
    };

    // Without intervening pointer activity, the delayed fallback becomes
    // terminal text at completion time and hides the pointer.
    pressPerformable();
    QCOMPARE(copies.count(), 1);
    QCOMPARE(forwarded.count(), 0);
    QCOMPARE(pane.cursor().shape(), Qt::IBeamCursor);
    const quint64 firstRequestId =
        copies.constFirst().constFirst().toULongLong();
    QVERIFY(firstRequestId != 0);
    completeUnavailable(firstRequestId);
    QTRY_COMPARE_WITH_TIMEOUT(forwarded.count(), 1, 1000);
    QCOMPARE(pane.cursor().shape(), Qt::BlankCursor);

    QHoverEvent reveal(QEvent::HoverMove, secondPosition, secondPosition,
                       firstPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &reveal);
    QCOMPARE(pane.cursor().shape(), Qt::IBeamCursor);

    QKeyEvent firstRelease(QEvent::KeyRelease, Qt::Key_X, Qt::AltModifier,
                           QStringLiteral("x"));
    QCoreApplication::sendEvent(&pane, &firstRelease);
    const int beforeSecondFallback = forwarded.count();

    // The same unavailable fallback still reaches the terminal, but a real
    // pointer move while it is pending starts a newer activity epoch. Its
    // delayed concealment must not override that user activity.
    pressPerformable();
    QCOMPARE(copies.count(), 2);
    QCOMPARE(forwarded.count(), beforeSecondFallback);
    const quint64 secondRequestId =
        copies.constLast().constFirst().toULongLong();
    QVERIFY(secondRequestId != 0);
    QVERIFY(secondRequestId != firstRequestId);
    QHoverEvent activity(QEvent::HoverMove, thirdPosition, thirdPosition,
                         secondPosition, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &activity);
    completeUnavailable(secondRequestId);
    QTRY_COMPARE_WITH_TIMEOUT(forwarded.count(), beforeSecondFallback + 1,
                              1000);
    QCOMPARE(pane.cursor().shape(), Qt::IBeamCursor);

    QKeyEvent secondRelease(QEvent::KeyRelease, Qt::Key_X, Qt::AltModifier,
                            QStringLiteral("x"));
    QCoreApplication::sendEvent(&pane, &secondRelease);
    const int beforePreEnableFallback = forwarded.count();

    // Enabling the policy does not change the visible cursor, but it must
    // invalidate a fallback from a key that was pressed while hiding was
    // disabled. Otherwise that older event would gain eligibility
    // retroactively when its worker result arrives.
    LaunchOptions disabled = options;
    disabled.mouseHideWhileTyping = false;
    pane.applyRuntimeOptions(disabled);
    pressPerformable();
    QCOMPARE(copies.count(), 3);
    QCOMPARE(forwarded.count(), beforePreEnableFallback);
    const quint64 preEnableRequestId =
        copies.constLast().constFirst().toULongLong();
    QVERIFY(preEnableRequestId != 0);
    QVERIFY(preEnableRequestId != secondRequestId);
    pane.applyRuntimeOptions(options);
    QCOMPARE(pane.cursor().shape(), Qt::IBeamCursor);
    completeUnavailable(preEnableRequestId);
    QTRY_COMPARE_WITH_TIMEOUT(forwarded.count(), beforePreEnableFallback + 1,
                              1000);
    QCOMPARE(pane.cursor().shape(), Qt::IBeamCursor);
}

void TerminalPaneTest::restoresHyperlinkPointerAfterTyping()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.mouseHideWhileTyping = true;
    useSystemFixedFont(options);

    const TerminalCellMetrics metrics = terminalCellMetrics(options.typography);
    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    pane.setSize(QSizeF(metrics.cellWidth * 2.0, metrics.cellHeight * 2.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy hyperlinkQueries(controller,
                                &TerminalController::hyperlinkQueryRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    TerminalUpdate update;
    update.columns = 1;
    update.rows = 1;
    update.fullFrame = true;
    update.contentRevision = 1;
    TerminalRowUpdate row;
    row.row = 0;
    row.cells.resize(1);
    row.cells[0].text = QStringLiteral("L");
    row.cells[0].hasHyperlink = true;
    update.dirtyRows.append(std::move(row));
    controller->terminalUpdated(update);

    QKeyEvent controlPress(QEvent::KeyPress, Qt::Key_Control,
                           Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &controlPress);
    forwarded.clear();
    const QPointF linkPosition(metrics.cellWidth * 0.5,
                               metrics.cellHeight * 0.5);
    QHoverEvent initialHover(QEvent::HoverMove, linkPosition, linkPosition,
                             linkPosition, Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &initialHover);
    QCOMPARE(hyperlinkQueries.count(), 1);

    const QByteArray uri("https://example.test/hidden-pointer");
    controller->hyperlinkResolved(
        update.contentRevision, TerminalHyperlinkState::Visible,
        TerminalLinkKind::Osc8, uri, QPoint(0, 0), {QPoint(0, 0)});
    QCOMPARE(pane.cursor().shape(), Qt::PointingHandCursor);

    // Ctrl remains held so the accepted hyperlink lease stays active while
    // terminal-bound text hides its presentation.
    QKeyEvent textPress(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier,
                        QStringLiteral("a"));
    QCoreApplication::sendEvent(&pane, &textPress);
    QCOMPARE(forwarded.count(), 1);
    QCOMPARE(pane.cursor().shape(), Qt::BlankCursor);

    // A late link result may update the logical pointer shape, but it must
    // not bypass typing concealment before actual pointer movement.
    controller->hyperlinkResolved(
        update.contentRevision, TerminalHyperlinkState::Visible,
        TerminalLinkKind::Osc8, uri, QPoint(0, 0), {QPoint(0, 0)});
    QCOMPARE(pane.cursor().shape(), Qt::BlankCursor);

    QHoverEvent restoringHover(
        QEvent::HoverMove, linkPosition + QPointF(1.0, 0.0),
        linkPosition + QPointF(1.0, 0.0), linkPosition, Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &restoringHover);
    QCOMPARE(pane.cursor().shape(), Qt::PointingHandCursor);

    QKeyEvent secondTextPress(QEvent::KeyPress, Qt::Key_B, Qt::ControlModifier,
                              QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &secondTextPress);
    QCOMPARE(pane.cursor().shape(), Qt::BlankCursor);

    // Clearing the logical link while concealed must not expose an
    // intermediate cursor. The next real movement reveals the inherited
    // default because no accepted hyperlink remains.
    controller->hyperlinkResolved(update.contentRevision,
                                  TerminalHyperlinkState::Stale,
                                  TerminalLinkKind::Osc8, {}, QPoint(0, 0), {});
    QCOMPARE(pane.cursor().shape(), Qt::BlankCursor);
    QHoverEvent revealWithoutLink(
        QEvent::HoverMove, linkPosition + QPointF(2.0, 0.0),
        linkPosition + QPointF(2.0, 0.0), linkPosition + QPointF(1.0, 0.0),
        Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &revealWithoutLink);
    QCOMPARE(pane.cursor().shape(), Qt::IBeamCursor);
}

void TerminalPaneTest::resolvesMinimumContrastWithShaderOrdering()
{
    QColor faintWhite(Qt::white);
    faintWhite.setAlpha(64);

    // The default threshold is a byte-for-byte no-op, including alpha.
    QCOMPARE(terminalMinimumContrastColorForTest(faintWhite, Qt::black,
                                                 Qt::black, 1.0),
             faintWhite);

    // Ghostty premultiplies faint alpha before measuring. The quarter-opaque
    // white is below 7:1 and correction deliberately becomes opaque.
    const QColor correctedFaint = terminalMinimumContrastColorForTest(
        faintWhite, Qt::black, Qt::black, 7.0);
    QCOMPARE(correctedFaint, QColor(Qt::white));
    QCOMPARE(correctedFaint.alpha(), 255);

    QCOMPARE(terminalMinimumContrastColorForTest(
                 QColor(QStringLiteral("#333333")), Qt::black, Qt::black, 7.0),
             QColor(Qt::white));
    QCOMPARE(terminalMinimumContrastColorForTest(
                 QColor(QStringLiteral("#dddddd")), Qt::white, Qt::white, 7.0),
             QColor(Qt::black));

    // Cell alpha is composited in linear premultiplied space before the
    // comparison. Half-transparent black over global white is a midtone, so
    // black—not the white that an opaque-black shortcut would choose—is the
    // stronger replacement.
    QCOMPARE(terminalMinimumContrastColorForTest(
                 QColor(QStringLiteral("#333333")), QColor(0, 0, 0, 128),
                 Qt::white, 7.0),
             QColor(Qt::black));

    QColor translucentWhite(Qt::white);
    translucentWhite.setAlpha(128);
    QColor defaultCell(Qt::black);
    defaultCell.setAlpha(0);
    // Default cells contribute no second layer. Ghostty therefore compares
    // against the quantized, premultiplied pane-wide background, even though
    // the compositor color behind the window is unknowable.
    QCOMPARE(terminalMinimumContrastColorForTest(
                 QColor(QStringLiteral("#333333")), defaultCell,
                 translucentWhite, 7.0),
             QColor(Qt::black));

    // Sufficient contrast and the exact maximum boundary retain the input.
    QCOMPARE(terminalMinimumContrastColorForTest(Qt::cyan, Qt::black, Qt::black,
                                                 7.0),
             QColor(Qt::cyan));
    QCOMPARE(terminalMinimumContrastColorForTest(Qt::white, Qt::black,
                                                 Qt::black, 21.0),
             QColor(Qt::white));
}

void TerminalPaneTest::rendersBackgroundOpacityAndReloadsInPlace()
{
    qRegisterMetaType<TerminalSearchUpdate>();
    qRegisterMetaType<TerminalUpdate>();

    const QColor globalBackground(QStringLiteral("#203040"));
    const QColor explicitBackground(QStringLiteral("#804020"));
    const QColor inverseBackground(QStringLiteral("#106030"));
    const QColor selectionBackground(QStringLiteral("#405080"));
    const QColor searchBackground(QStringLiteral("#805040"));
    const QColor selectedSearchBackground(QStringLiteral("#408050"));

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 10"),
    };
    options.hold = true;
    useSystemFixedFont(options);
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = globalBackground;
    options.appearance.selectionBackground =
        TerminalColorValue::fromColor(selectionBackground);
    options.appearance.searchBackground =
        TerminalColorValue::fromColor(searchBackground);
    options.appearance.searchSelectedBackground =
        TerminalColorValue::fromColor(selectedSearchBackground);
    options.background = {
        .opacity = 0.5,
        .opacityCells = false,
    };

    const TerminalCellMetrics metrics = terminalCellMetrics(options.typography);
    constexpr int columns = 7;
    constexpr int rows = 1;
    QQuickWindow window;
    window.setColor(Qt::transparent);
    window.resize(qCeil(metrics.cellWidth * columns),
                  qCeil(metrics.cellHeight * 3));
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *const controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);

    TerminalUpdate update;
    update.columns = columns;
    update.rows = rows;
    update.fullFrame = true;
    update.colorsChanged = true;
    update.foreground = Qt::white;
    update.background = globalBackground;
    update.cursorColor = Qt::white;
    update.cursorChanged = true;
    update.cursorVisible = false;
    update.contentRevision = 401;
    TerminalRowUpdate row;
    row.row = 0;
    row.cells.resize(columns);
    for (TerminalCell &cell : row.cells) {
        cell.text = QStringLiteral("A");
        cell.foreground = Qt::white;
        cell.background = globalBackground;
        cell.underlineColor = Qt::white;
    }
    // A default cell and an explicit cell may resolve to the same RGB but
    // must remain distinguishable for cell-opacity policy.
    row.cells[1].backgroundExplicit = true;
    row.cells[2].background = explicitBackground;
    row.cells[2].backgroundExplicit = true;
    row.cells[3].background = inverseBackground;
    row.cells[3].inverse = true;
    row.cells[4].selected = true;
    update.dirtyRows.append(std::move(row));
    controller->terminalUpdated(update);

    TerminalSearchUpdate search;
    search.generation = 1;
    search.contentRevision = update.contentRevision;
    search.active = true;
    search.complete = true;
    search.totalMatches = 2;
    search.selectedMatch = 0;
    search.columns = columns;
    search.rows = rows;
    search.visibleCellMask =
        cellMask(columns, rows, {QPoint(5, 0), QPoint(6, 0)});
    search.selectedCellMask = cellMask(columns, rows, {QPoint(6, 0)});
    controller->searchUpdated(search);

    const auto paintedProbe = [&] {
        pane->update();
        (void)window.grabWindow();
        return terminalPaneRenderProbe(pane);
    };
    const TerminalPaneRenderProbeSnapshot opaqueCells = paintedProbe();
    QCOMPARE(opaqueCells.baseBackground, QColor(32, 48, 64, 128));
    QCOMPARE(opaqueCells.cellBackgrounds.size(), columns);
    QCOMPARE(opaqueCells.cellBackgrounds.at(0).alpha(), 0);
    QCOMPARE(opaqueCells.cellBackgrounds.at(1),
             QColor(32, 48, 64, 255));
    QCOMPARE(opaqueCells.cellBackgrounds.at(2),
             QColor(128, 64, 32, 255));
    QCOMPARE(opaqueCells.cellBackgrounds.at(3),
             QColor(16, 96, 48, 255));
    QCOMPARE(opaqueCells.cellBackgrounds.at(4), selectionBackground);
    QCOMPARE(opaqueCells.cellBackgrounds.at(5), searchBackground);
    QCOMPARE(opaqueCells.cellBackgrounds.at(6), selectedSearchBackground);
    const quint64 rootSerial = opaqueCells.rootSerial;
    const QVector<quint64> rowBuildCounts = opaqueCells.rowBuildCounts;
    const QVector<quint64> rowSolidBuildCounts =
        opaqueCells.rowSolidBuildCounts;

    // Pane opacity affects only the backdrop at the default contrast threshold
    // when explicit cell opacity is disabled.
    LaunchOptions paneOpacityOnly = options;
    paneOpacityOnly.background.opacity = 0.25;
    pane->applyRuntimeOptions(paneOpacityOnly);
    const TerminalPaneRenderProbeSnapshot changedPaneOpacity = paintedProbe();
    QCOMPARE(changedPaneOpacity.baseBackground, QColor(32, 48, 64, 64));
    QCOMPARE(changedPaneOpacity.rowSolidBuildCounts, rowSolidBuildCounts);
    QCOMPARE(changedPaneOpacity.solidCellVisitCount,
             opaqueCells.solidCellVisitCount);

    LaunchOptions translucentCells = options;
    translucentCells.background.opacityCells = true;
    pane->applyRuntimeOptions(translucentCells);
    const TerminalPaneRenderProbeSnapshot translucent = paintedProbe();
    QCOMPARE(translucent.rootSerial, rootSerial);
    QCOMPARE(pane->findChild<TerminalController *>(), controller);
    QCOMPARE(translucent.rowBuildCounts, rowBuildCounts);
    QCOMPARE(translucent.baseBackground.alpha(), 128);
    QCOMPARE(translucent.cellBackgrounds.at(0).alpha(), 0);
    // Pinned Ghostty truncates cell alpha but rounds the global alpha.
    QCOMPARE(translucent.cellBackgrounds.at(1).alpha(), 127);
    QCOMPARE(translucent.cellBackgrounds.at(2).alpha(), 127);
    for (int index = 3; index < columns; ++index) {
        QCOMPARE(translucent.cellBackgrounds.at(index).alpha(), 255);
    }

    LaunchOptions zeroOpacity = translucentCells;
    zeroOpacity.background.opacity = 0.0;
    pane->applyRuntimeOptions(zeroOpacity);
    const TerminalPaneRenderProbeSnapshot transparent = paintedProbe();
    QCOMPARE(transparent.rootSerial, rootSerial);
    QCOMPARE(transparent.rowBuildCounts, rowBuildCounts);
    QCOMPARE(transparent.baseBackground.alpha(), 0);
    for (int index = 0; index <= 2; ++index) {
        QCOMPARE(transparent.cellBackgrounds.at(index).alpha(), 0);
    }
    for (int index = 3; index < columns; ++index) {
        QCOMPARE(transparent.cellBackgrounds.at(index).alpha(), 255);
    }

    zeroOpacity.background.opacityCells = false;
    pane->applyRuntimeOptions(zeroOpacity);
    const TerminalPaneRenderProbeSnapshot explicitOpaque = paintedProbe();
    QCOMPARE(explicitOpaque.rootSerial, rootSerial);
    QCOMPARE(explicitOpaque.baseBackground.alpha(), 0);
    QCOMPARE(explicitOpaque.cellBackgrounds.at(0).alpha(), 0);
    QCOMPARE(explicitOpaque.cellBackgrounds.at(1).alpha(), 255);
    QCOMPARE(explicitOpaque.cellBackgrounds.at(2).alpha(), 255);
    for (int index = 3; index < columns; ++index) {
        QCOMPARE(explicitOpaque.cellBackgrounds.at(index).alpha(), 255);
    }

    // Minimum-contrast colors are retained in row text nodes. Changing only
    // explicit-cell alpha must invalidate those nodes even though the rounded
    // pane base is unchanged.
    TerminalUpdate contrastUpdate = update;
    contrastUpdate.background = Qt::white;
    ++contrastUpdate.contentRevision;
    for (TerminalCell &cell : contrastUpdate.dirtyRows.first().cells) {
        cell.background = Qt::white;
    }
    contrastUpdate.dirtyRows.first().cells[1].backgroundExplicit = true;
    contrastUpdate.dirtyRows.first().cells[2].background = Qt::black;
    contrastUpdate.dirtyRows.first().cells[2].backgroundExplicit = true;
    controller->terminalUpdated(contrastUpdate);

    LaunchOptions contrastOpaqueCells = options;
    contrastOpaqueCells.appearance.minimumContrast = 7.0;
    pane->applyRuntimeOptions(contrastOpaqueCells);
    const TerminalPaneRenderProbeSnapshot beforeContrastReload =
        paintedProbe();
    QCOMPARE(beforeContrastReload.baseBackground,
             QColor(255, 255, 255, 128));
    QCOMPARE(beforeContrastReload.cellBackgrounds.at(2), QColor(Qt::black));
    QCOMPARE(beforeContrastReload.glyphForegrounds.at(2), QColor(Qt::white));

    LaunchOptions contrastTranslucentCells = contrastOpaqueCells;
    contrastTranslucentCells.background.opacityCells = true;
    pane->applyRuntimeOptions(contrastTranslucentCells);
    const TerminalPaneRenderProbeSnapshot afterContrastReload =
        paintedProbe();
    QCOMPARE(afterContrastReload.rootSerial, rootSerial);
    QCOMPARE(afterContrastReload.baseBackground,
             beforeContrastReload.baseBackground);
    QCOMPARE(afterContrastReload.cellBackgrounds.at(2),
             QColor(0, 0, 0, 127));
    QCOMPARE(afterContrastReload.glyphForegrounds.at(2), QColor(Qt::black));
    QVERIFY(allVisibleRowsRebuilt(beforeContrastReload,
                                  afterContrastReload));

    window.close();
    delete pane;
}

void TerminalPaneTest::
    retainsBackgroundImageAcrossOptionReloadAndDecodeFailure()
{
    const QString temporaryRoot =
        QDir::current().filePath(QStringLiteral("tmp"));
    QVERIFY(QDir().mkpath(temporaryRoot));
    QTemporaryDir temporary(
        QDir(temporaryRoot)
            .filePath(QStringLiteral("terminal-pane-backdrop-XXXXXX")));
    QVERIFY(temporary.isValid());

    constexpr QSize imageSize(32, 16);
    const QString imagePath =
        temporary.filePath(QStringLiteral("backdrop.png"));
    QImage image(imageSize, QImage::Format_RGBA8888);
    image.fill(QColor(QStringLiteral("#40a0e0")));
    QVERIFY(image.save(imagePath, "PNG"));

    const QString invalidPath =
        temporary.filePath(QStringLiteral("invalid-image.bin"));
    QFile invalidFile(invalidPath);
    QVERIFY(invalidFile.open(QIODevice::WriteOnly));
    QCOMPARE(invalidFile.write(QByteArrayLiteral("not an image")),
             qsizetype{12});
    invalidFile.close();

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 10"),
    };
    options.hold = true;
    useSystemFixedFont(options);
    options.appearance.backgroundColor = QColor(QStringLiteral("#102030"));
    options.background.image = {
        .path =
            GhosttyConfigPath{
                .path = imagePath,
                .optional = false,
            },
        .opacity = 0.75,
        .position = TerminalBackgroundImagePosition::Center,
        .fit = TerminalBackgroundImageFit::Stretch,
        .repeat = false,
    };
    options.padding = {
        .horizontal = {.leadingPoints = 12, .trailingPoints = 18},
        .vertical = {.leadingPoints = 9, .trailingPoints = 15},
    };

    QQuickWindow window;
    window.setColor(Qt::transparent);
    window.resize(320, 180);
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    const QPointer<TerminalPane> guardedPane(pane);
    TerminalController *const controller =
        pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(controller->sessionStarted(), 3000);
    QThread *const workerThread = controller->findChild<QThread *>();
    QVERIFY(workerThread != nullptr);

    const auto paintedProbe = [&] {
        pane->update();
        const QImage frame = window.grabWindow();
        if (frame.isNull()) return TerminalPaneRenderProbeSnapshot{};
        return terminalPaneRenderProbe(pane);
    };
    QTRY_VERIFY_WITH_TIMEOUT(paintedProbe().backgroundImageAssetSerial != 0,
                             3000);
    const TerminalPaneRenderProbeSnapshot loaded = paintedProbe();
    const quint64 assetSerial = loaded.backgroundImageAssetSerial;
    const quint64 rootSerial = loaded.rootSerial;
    QVERIFY(assetSerial != 0);
    QVERIFY(rootSerial != 0);
    QCOMPARE(loaded.backgroundImageRect, pane->boundingRect());
    QCOMPARE(loaded.backgroundImageSourceRect,
             QRectF(QPointF{}, QSizeF(imageSize)));
    QVERIFY(
        std::ranges::all_of(loaded.backdropBaseRects,
                            [](const QRectF &rect) { return rect.isEmpty(); }));

    const auto layout = terminalViewportLayout({
        .surfaceSize = pane->size(),
        .cellSize = QSizeF(loaded.metrics.cellWidth, loaded.metrics.cellHeight),
        .devicePixelRatio = window.devicePixelRatio(),
        .padding = options.padding,
    });
    QVERIFY(layout.has_value());
    QVERIFY(layout->gridRect.left() > pane->boundingRect().left());
    QVERIFY(layout->gridRect.top() > pane->boundingRect().top());
    QVERIFY(layout->gridRect.right() < pane->boundingRect().right());
    QVERIFY(layout->gridRect.bottom() < pane->boundingRect().bottom());
    QVERIFY(loaded.backgroundImageRect.contains(layout->gridRect));

    QSignalSpy resized(controller, &TerminalController::resizeRequested);
    QSignalSpy runtime(controller,
                       &TerminalController::runtimeOptionsRequested);

    LaunchOptions optionReload = options;
    optionReload.background.image.opacity = 0.4;
    optionReload.background.image.position =
        TerminalBackgroundImagePosition::BottomRight;
    optionReload.background.image.fit = TerminalBackgroundImageFit::None;
    optionReload.background.image.repeat = true;
    pane->applyRuntimeOptions(optionReload);
    const TerminalPaneRenderProbeSnapshot reconfigured = paintedProbe();
    QCOMPARE(reconfigured.backgroundImageAssetSerial, assetSerial);
    QCOMPARE(reconfigured.backgroundImageRect, pane->boundingRect());
    QCOMPARE(reconfigured.rootSerial, rootSerial);
    QVERIFY(reconfigured.paintSerial > loaded.paintSerial);
    QCOMPARE(guardedPane.data(), pane);
    QCOMPARE(pane->findChild<TerminalController *>(), controller);
    QCOMPARE(controller->findChild<QThread *>(), workerThread);
    QCOMPARE(resized.count(), 0);
    QCOMPARE(runtime.count(), 0);

    LaunchOptions invalidReload = optionReload;
    invalidReload.background.image.path = GhosttyConfigPath{
        .path = invalidPath,
        .optional = false,
    };
    const QByteArray expectedWarning =
        QStringLiteral("Background image '%1' is not PNG or JPEG.")
            .arg(invalidPath)
            .toUtf8();
    QTest::ignoreMessage(QtWarningMsg, expectedWarning.constData());
    pane->applyRuntimeOptions(invalidReload);
    QTest::qWait(500);
    const TerminalPaneRenderProbeSnapshot retained = paintedProbe();
    QCOMPARE(retained.backgroundImageAssetSerial, assetSerial);
    QCOMPARE(retained.backgroundImageRect, pane->boundingRect());
    QCOMPARE(retained.rootSerial, rootSerial);
    QCOMPARE(pane->findChild<TerminalController *>(), controller);
    QCOMPARE(controller->findChild<QThread *>(), workerThread);
    QCOMPARE(resized.count(), 0);
    QCOMPARE(runtime.count(), 0);

    LaunchOptions cleared = invalidReload;
    cleared.background.image.path.reset();
    pane->applyRuntimeOptions(cleared);
    const TerminalPaneRenderProbeSnapshot withoutImage = paintedProbe();
    QCOMPARE(withoutImage.backgroundImageAssetSerial, quint64{0});
    QVERIFY(withoutImage.backgroundImageRect.isEmpty());
    QVERIFY(withoutImage.backgroundImageSourceRect.isEmpty());
    QCOMPARE(withoutImage.rootSerial, rootSerial);
    QCOMPARE(withoutImage.backdropBaseRects.size(), qsizetype{4});
    QCOMPARE(withoutImage.backdropBaseRects.constFirst(), pane->boundingRect());
    QVERIFY(
        std::ranges::all_of(withoutImage.backdropBaseRects.cbegin() + 1,
                            withoutImage.backdropBaseRects.cend(),
                            [](const QRectF &rect) { return rect.isEmpty(); }));
    QCOMPARE(pane->findChild<TerminalController *>(), controller);
    QCOMPARE(controller->findChild<QThread *>(), workerThread);
    QCOMPARE(resized.count(), 0);
    QCOMPARE(runtime.count(), 0);

    window.close();
    delete pane;
    QVERIFY(guardedPane.isNull());
}

void TerminalPaneTest::rendersMinimumContrastAndReloadsLive()
{
    qRegisterMetaType<TerminalSearchUpdate>();
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral("sleep 10"),
    };
    options.hold = true;
    useSystemFixedFont(options);
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;
    options.appearance.faintOpacity = 0.25;
    options.appearance.cursorOpacity = 0.25;
    options.appearance.cursorTextColor =
        TerminalColorValue::fromColor(Qt::green);
    options.appearance.boldColor = {
        .kind = TerminalBoldColorKind::Color,
        .color = QColor(QStringLiteral("#333333")),
    };
    options.appearance.selectionForeground =
        TerminalColorValue::fromColor(QColor(QStringLiteral("#eeeeee")));
    options.appearance.selectionBackground =
        TerminalColorValue::fromColor(Qt::white);
    options.appearance.searchForeground =
        TerminalColorValue::fromColor(QColor(QStringLiteral("#eeeeee")));
    options.appearance.searchBackground =
        TerminalColorValue::fromColor(Qt::white);

    const TerminalCellMetrics metrics = terminalCellMetrics(options.typography);
    constexpr int columns = 8;
    constexpr int rows = 2;
    constexpr qsizetype cellCount = columns * rows;
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(qCeil(metrics.cellWidth * columns),
                  qCeil(metrics.cellHeight * (rows + 1)));
    auto *pane = new TerminalPane(options, window.contentItem());
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);

    TerminalUpdate update;
    update.columns = columns;
    update.rows = rows;
    update.fullFrame = true;
    update.colorsChanged = true;
    update.foreground = Qt::white;
    update.background = Qt::black;
    update.cursorColor = QColor(QStringLiteral("#202020"));
    update.cursorColorExplicit = true;
    update.cursorChanged = true;
    update.cursorVisible = true;
    update.cursorColumn = 0;
    update.cursorRow = 1;
    update.cursorStyle = 1;
    update.contentRevision = 91;
    for (int row = 0; row < rows; ++row) {
        TerminalRowUpdate rowUpdate;
        rowUpdate.row = row;
        rowUpdate.cells.resize(columns);
        for (TerminalCell &cell : rowUpdate.cells) {
            cell.text = QStringLiteral("A");
            cell.foreground = QColor(QStringLiteral("#333333"));
            cell.background = Qt::black;
            cell.underlineColor = QColor(QStringLiteral("#333333"));
        }
        update.dirtyRows.append(std::move(rowUpdate));
    }

    update.dirtyRows[0].cells[1].foreground = Qt::cyan;

    TerminalCell &graphics = update.dirtyRows[0].cells[2];
    graphics.text = QString(QChar(0x2588));
    graphics.minimumContrastExemptGlyph = true;
    graphics.underlineStyle = TerminalUnderlineStyle::Single;
    graphics.strikeThrough = true;
    graphics.overline = true;

    TerminalCell &explicitUnderline = update.dirtyRows[0].cells[3];
    explicitUnderline.foreground = Qt::cyan;
    explicitUnderline.underlineStyle = TerminalUnderlineStyle::Single;
    explicitUnderline.underlineUsesForeground = false;

    update.dirtyRows[0].cells[4].selected = true;

    TerminalCell &faint = update.dirtyRows[0].cells[6];
    faint.foreground = Qt::white;
    faint.faint = true;

    TerminalCell &bold = update.dirtyRows[0].cells[7];
    bold.foreground = Qt::white;
    bold.bold = true;
    bold.styleForegroundSource = TerminalColorSource::Default;

    controller->terminalUpdated(update);
    TerminalSearchUpdate search;
    search.generation = 1;
    search.contentRevision = update.contentRevision;
    search.active = true;
    search.complete = true;
    search.totalMatches = 1;
    search.selectedMatch = -1;
    search.columns = columns;
    search.rows = rows;
    search.visibleCellMask = QBitArray(cellCount);
    search.selectedCellMask = QBitArray(cellCount);
    search.visibleCellMask.setBit(5);
    controller->searchUpdated(search);

    // Keep the synthetic frame stable while exercising appearance-only reload.
    QObject::disconnect(controller, &TerminalController::terminalUpdated, pane,
                        nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(
        terminalPaneRenderProbe(pane).glyphForegrounds.size() == cellCount,
        3000);
    const TerminalPaneRenderProbeSnapshot before =
        terminalPaneRenderProbe(pane);
    QCOMPARE(before.glyphForegrounds.at(0), QColor(QStringLiteral("#333333")));
    QCOMPARE(before.glyphForegrounds.at(6).alpha(), 64);
    QCOMPARE(before.cursorColor, QColor(32, 32, 32, 64));

    LaunchOptions reloaded = options;
    reloaded.appearance.minimumContrast = 7.0;
    pane->applyRuntimeOptions(reloaded);
    QTRY_VERIFY_WITH_TIMEOUT(
        terminalPaneRenderProbe(pane).paintSerial > before.paintSerial, 3000);
    const TerminalPaneRenderProbeSnapshot after = terminalPaneRenderProbe(pane);
    QCOMPARE(after.rootSerial, before.rootSerial);
    QVERIFY(allVisibleRowsRebuilt(before, after));

    QCOMPARE(after.glyphForegrounds.at(0), QColor(Qt::white));
    QCOMPARE(after.glyphForegrounds.at(1), QColor(Qt::cyan));
    QCOMPARE(after.glyphForegrounds.at(2), QColor(QStringLiteral("#333333")));
    QCOMPARE(after.decorationForegrounds.at(2), QColor(Qt::white));
    QCOMPARE(after.underlineColors.at(2), QColor(Qt::white));
    QCOMPARE(after.glyphForegrounds.at(3), QColor(Qt::cyan));
    QCOMPARE(after.underlineColors.at(3), QColor(Qt::white));
    QCOMPARE(after.glyphForegrounds.at(4), QColor(Qt::black));
    QCOMPARE(after.glyphForegrounds.at(5), QColor(Qt::black));
    QCOMPARE(after.glyphForegrounds.at(6), QColor(Qt::white));
    QCOMPARE(after.glyphForegrounds.at(6).alpha(), 255);
    QCOMPARE(after.glyphForegrounds.at(7), QColor(Qt::white));

    // Minimum contrast precedes the block cursor-text override, while the
    // low-contrast cursor sprite itself snaps to opaque white.
    QCOMPARE(after.glyphForegrounds.at(columns), QColor(Qt::green));
    QCOMPARE(after.decorationForegrounds.at(columns), QColor(Qt::green));
    QCOMPARE(after.cursorColor, QColor(Qt::white));
    QCOMPARE(after.cursorColor.alpha(), 255);

    window.close();
    delete pane;
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
    useSystemFixedFont(options);
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

    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography);
    const qreal cellWidth = metrics.cellWidth;
    const qreal cellHeight = metrics.cellHeight;

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
    QVERIFY(decorationPixels[3] > 0);
    QVERIFY(decorationPixels[4] > 0);
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

void TerminalPaneTest::rendersResolvedTypographyAndPhysicalGeometry()
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
    useSystemFixedFont(options);
    auto &modifiers = options.typography.metricModifiers;
    modifiers[TerminalMetric::FontBaseline] =
        TerminalMetricModifiers::Absolute{.pixels = 2};
    modifiers[TerminalMetric::UnderlinePosition] =
        TerminalMetricModifiers::Absolute{.pixels = 1};
    modifiers[TerminalMetric::UnderlineThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 1};
    modifiers[TerminalMetric::StrikethroughPosition] =
        TerminalMetricModifiers::Absolute{.pixels = -1};
    modifiers[TerminalMetric::StrikethroughThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 2};
    modifiers[TerminalMetric::OverlinePosition] =
        TerminalMetricModifiers::Absolute{.pixels = -2};
    modifiers[TerminalMetric::OverlineThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 1};
    modifiers[TerminalMetric::CursorThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 2};
    modifiers[TerminalMetric::CursorHeight] =
        TerminalMetricModifiers::Absolute{.pixels = -4};

    QQuickWindow window;
    const qreal dpr = window.devicePixelRatio();
    const TerminalCellMetrics expected =
        terminalCellMetrics(options.typography, dpr);
    constexpr int columns = 4;
    constexpr int rows = 2;
    window.setColor(Qt::black);
    window.resize(
        qCeil(expected.cellWidth * columns),
        qCeil(expected.cellHeight * rows));
    auto *pane = new TerminalPane(
        options, window.contentItem(), std::nullopt,
        TerminalSessionStartMode::Deferred);
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);

    TerminalUpdate update;
    update.columns = columns;
    update.rows = rows;
    update.fullFrame = true;
    update.foreground = Qt::white;
    update.background = Qt::black;
    update.cursorColor = Qt::magenta;
    update.cursorColorExplicit = true;
    update.cursorChanged = true;
    update.cursorVisible = true;
    update.cursorBlinking = false;
    update.cursorColumn = 3;
    update.cursorRow = 0;
    update.cursorStyle = 1;
    update.cursorColumnSpan = 1;
    update.contentRevision = 1;
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
    const std::array<std::pair<bool, bool>, 4> roles{{
        {false, false},
        {true, false},
        {false, true},
        {true, true},
    }};
    for (std::size_t index = 0; index < roles.size(); ++index) {
        TerminalCell &cell =
            update.dirtyRows[0].cells[static_cast<qsizetype>(index)];
        cell.text = QStringLiteral("M");
        cell.bold = roles[index].first;
        cell.italic = roles[index].second;
    }
    TerminalCell &decorated = update.dirtyRows[0].cells[0];
    decorated.underlineStyle = TerminalUnderlineStyle::Single;
    decorated.strikeThrough = true;
    decorated.overline = true;

    controller->terminalUpdated(update);
    QVERIFY(!window.grabWindow().isNull());

    const qreal underlinePosition = std::min(
        expected.underlinePosition, expected.underlineMaximumPosition);
    const qreal overlinePosition = std::max(
        expected.overlinePosition, expected.overlineMinimumPosition);
    const qreal cursorTop = expected.cursorTop;
    const qreal cursorLeft =
        static_cast<qreal>(update.cursorColumn) * expected.cellWidth;

    const TerminalPaneRenderProbeSnapshot block =
        terminalPaneRenderProbe(pane);
    QVERIFY(block.metrics == expected);
    QVERIFY(block.renderFonts == expected.fontProgram->fonts);
    QVERIFY(
        block.fontRoleCellCounts
        == (std::array<quint64, 4>{1, 1, 1, 1}));
    QVERIFY(!block.renderFonts[
        terminalEnumIndex(TerminalFontRole::Regular)].bold());
    QVERIFY(!block.renderFonts[
        terminalEnumIndex(TerminalFontRole::Regular)].italic());
    QVERIFY(block.renderFonts[
        terminalEnumIndex(TerminalFontRole::Bold)].bold());
    QVERIFY(!block.renderFonts[
        terminalEnumIndex(TerminalFontRole::Bold)].italic());
    QVERIFY(!block.renderFonts[
        terminalEnumIndex(TerminalFontRole::Italic)].bold());
    QVERIFY(block.renderFonts[
        terminalEnumIndex(TerminalFontRole::Italic)].italic());
    QVERIFY(block.renderFonts[
        terminalEnumIndex(TerminalFontRole::BoldItalic)].bold());
    QVERIFY(block.renderFonts[
        terminalEnumIndex(TerminalFontRole::BoldItalic)].italic());
    QCOMPARE(block.underlineRects.size(), 1);
    QCOMPARE(
        block.underlineRects.constFirst(),
        QRectF(0.0, underlinePosition, expected.cellWidth,
               expected.underlineThickness));
    QCOMPARE(block.strikethroughRects.size(), 1);
    QCOMPARE(
        block.strikethroughRects.constFirst(),
        QRectF(0.0, expected.strikethroughPosition,
               expected.cellWidth, expected.strikethroughThickness));
    QCOMPARE(block.overlineRects.size(), 1);
    QCOMPARE(
        block.overlineRects.constFirst(),
        QRectF(0.0, overlinePosition, expected.cellWidth,
               expected.overlineThickness));
    QCOMPARE(block.cursorRects,
             QVector<QRectF>({
                 QRectF(cursorLeft, cursorTop, expected.cellWidth,
                        expected.cursorHeight),
             }));

    const auto renderCursor = [&](int style) -> QVector<QRectF> {
        TerminalUpdate cursor;
        cursor.columns = columns;
        cursor.rows = rows;
        cursor.cursorChanged = true;
        cursor.cursorVisible = true;
        cursor.cursorBlinking = false;
        cursor.cursorColumn = update.cursorColumn;
        cursor.cursorRow = update.cursorRow;
        cursor.cursorStyle = style;
        cursor.cursorColumnSpan = 1;
        cursor.contentRevision = ++update.contentRevision;
        controller->terminalUpdated(cursor);
        if (window.grabWindow().isNull()) {
            return {};
        }
        return terminalPaneRenderProbe(pane).cursorRects;
    };

    QCOMPARE(
        renderCursor(0),
        QVector<QRectF>({
            QRectF(cursorLeft + expected.cursorBarLeft, cursorTop,
                   expected.cursorThickness, expected.cursorHeight),
        }));
    QCOMPARE(
        renderCursor(2),
        QVector<QRectF>({
            QRectF(cursorLeft, underlinePosition, expected.cellWidth,
                   expected.cursorThickness),
        }));

    const qreal bottomOffset = std::max(
        qreal{0.0}, expected.cursorHeight - expected.cursorThickness);
    const qreal rightOffset = std::max(
        qreal{0.0}, expected.cellWidth - expected.cursorThickness);
    const qreal innerTopOffset = std::min(
        expected.cursorThickness, expected.cursorHeight);
    const qreal innerHeight = std::max(
        qreal{0.0},
        expected.cursorHeight - 2.0 * expected.cursorThickness);
    const qreal horizontalThickness = std::min(
        expected.cursorThickness, expected.cursorHeight);
    const qreal verticalThickness = std::min(
        expected.cursorThickness, expected.cellWidth);
    const QVector<QRectF> hollowCursor = renderCursor(3);
    QCOMPARE(
        hollowCursor,
        QVector<QRectF>({
            QRectF(cursorLeft, cursorTop, expected.cellWidth,
                   horizontalThickness),
            QRectF(cursorLeft, cursorTop + bottomOffset,
                   expected.cellWidth, horizontalThickness),
            QRectF(cursorLeft, cursorTop + innerTopOffset,
                   verticalThickness, innerHeight),
            QRectF(cursorLeft + rightOffset,
                   cursorTop + innerTopOffset,
                   verticalThickness, innerHeight),
        }));
    const QRectF outerCursor(
        cursorLeft, cursorTop, expected.cellWidth, expected.cursorHeight);
    const qreal hollowInnerWidth = std::max(
        qreal{0.0},
        expected.cellWidth - 2.0 * expected.cursorThickness);
    const qreal hollowInnerHeight = std::max(
        qreal{0.0},
        expected.cursorHeight - 2.0 * expected.cursorThickness);
    QVERIFY(rectanglesArePairwiseDisjoint(hollowCursor));
    QVERIFY(rectanglesFitInside(hollowCursor, outerCursor));
    QVERIFY(qFuzzyCompare(
        totalRectangleArea(hollowCursor) + 1.0,
        rectangleArea(outerCursor)
            - hollowInnerWidth * hollowInnerHeight + 1.0));

    QInputMethodEvent preedit(
        QStringLiteral("composition"),
        QList<QInputMethodEvent::Attribute>{});
    QCoreApplication::sendEvent(pane, &preedit);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot withPreedit =
        terminalPaneRenderProbe(pane);
    QCOMPARE(withPreedit.underlineRects.size(), 2);
    QVERIFY(withPreedit.overlayTextNodeSerial != 0);
    QVERIFY(withPreedit.overlayTextBuildCount > 0);
    const QRectF preeditUnderline = withPreedit.underlineRects.constLast();
    QCOMPARE(preeditUnderline.left(), cursorLeft);
    QCOMPARE(preeditUnderline.top(), underlinePosition);
    QCOMPARE(preeditUnderline.height(), expected.underlineThickness);

    pane->update();
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot repeatedPreedit =
        terminalPaneRenderProbe(pane);
    QCOMPARE(repeatedPreedit.overlayTextNodeSerial,
             withPreedit.overlayTextNodeSerial);
    QCOMPARE(repeatedPreedit.overlayTextBuildCount,
             withPreedit.overlayTextBuildCount);

    QInputMethodEvent clearedPreedit(QString{}, {});
    QCoreApplication::sendEvent(pane, &clearedPreedit);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot cleared =
        terminalPaneRenderProbe(pane);
    QCOMPARE(cleared.overlayTextNodeSerial, withPreedit.overlayTextNodeSerial);
    QCOMPARE(cleared.overlayTextBuildCount,
             withPreedit.overlayTextBuildCount + 1);

    QInputMethodEvent restoredPreedit(QStringLiteral("restored"), {});
    QCoreApplication::sendEvent(pane, &restoredPreedit);
    QVERIFY(!window.grabWindow().isNull());
    const TerminalPaneRenderProbeSnapshot restored =
        terminalPaneRenderProbe(pane);
    QCOMPARE(restored.overlayTextNodeSerial, withPreedit.overlayTextNodeSerial);
    QCOMPARE(restored.overlayTextBuildCount,
             withPreedit.overlayTextBuildCount + 2);

    LaunchOptions oversizedCursor = options;
    oversizedCursor.typography.metricModifiers[
        TerminalMetric::CursorThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 10'000};
    pane->applyRuntimeOptions(oversizedCursor);
    const TerminalCellMetrics oversizedMetrics = terminalCellMetrics(
        oversizedCursor.typography, window.devicePixelRatio());
    const QVector<QRectF> oversizedHollow = renderCursor(3);
    const QRectF oversizedOuterCursor(
        static_cast<qreal>(update.cursorColumn)
            * oversizedMetrics.cellWidth,
        oversizedMetrics.cursorTop,
        oversizedMetrics.cellWidth, oversizedMetrics.cursorHeight);
    QCOMPARE(
        oversizedHollow,
        QVector<QRectF>({oversizedOuterCursor}));
    QVERIFY(rectanglesArePairwiseDisjoint(oversizedHollow));
    QVERIFY(rectanglesFitInside(
        oversizedHollow, oversizedOuterCursor));
    QCOMPARE(
        totalRectangleArea(oversizedHollow),
        rectangleArea(oversizedOuterCursor));

    window.close();
    delete pane;
}

void TerminalPaneTest::clipsDecorationAndCursorSprites()
{
    qRegisterMetaType<TerminalUpdate>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.hold = true;
    useSystemFixedFont(options);

    QQuickWindow window;
    const qreal dpr = window.devicePixelRatio();
    const TerminalCellMetrics initial =
        terminalCellMetrics(options.typography, dpr);
    window.setColor(Qt::black);
    window.resize(
        qCeil(initial.cellWidth * 2.0),
        qCeil(initial.cellHeight * 2.0));
    auto *pane = new TerminalPane(
        options, window.contentItem(), std::nullopt,
        TerminalSessionStartMode::Deferred);
    pane->setSize(window.size());
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
    pane->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(pane->hasActiveFocus(), 1000);

    quint64 contentRevision = 0;
    const auto render =
        [&](const LaunchOptions &runtime,
            TerminalUnderlineStyle underlineStyle,
            bool strikethrough = false,
            bool overline = false,
            int cursorStyle = -1) {
            pane->applyRuntimeOptions(runtime);

            TerminalUpdate update;
            update.columns = 1;
            update.rows = 1;
            update.fullFrame = true;
            update.foreground = Qt::white;
            update.background = Qt::black;
            update.cursorColor = Qt::magenta;
            update.cursorColorExplicit = true;
            update.cursorChanged = true;
            update.cursorVisible = cursorStyle >= 0;
            update.cursorBlinking = false;
            update.cursorColumn = 0;
            update.cursorRow = 0;
            update.cursorStyle = std::max(cursorStyle, 0);
            update.cursorColumnSpan = 1;
            update.contentRevision = ++contentRevision;

            TerminalRowUpdate row;
            row.row = 0;
            row.cells.resize(1);
            row.cells[0].foreground = Qt::white;
            row.cells[0].background = Qt::black;
            row.cells[0].underlineStyle = underlineStyle;
            row.cells[0].underlineUsesForeground = true;
            row.cells[0].strikeThrough = strikethrough;
            row.cells[0].overline = overline;
            update.dirtyRows.append(std::move(row));

            controller->terminalUpdated(update);
            if (window.grabWindow().isNull()) {
                return TerminalPaneRenderProbeSnapshot{};
            }
            return terminalPaneRenderProbe(pane);
        };
    const auto physical = [dpr](qreal logical) {
        return qRound64(logical * dpr);
    };
    const auto approximatelySame = [](qreal left, qreal right) {
        return std::abs(left - right) < 1e-6;
    };

    // Every underline style owns its bound. A large configured position must
    // not be pre-clamped with the single-line rule before the style-specific
    // sprite function sees it.
    LaunchOptions positioned = options;
    positioned.typography.metricModifiers[
        TerminalMetric::UnderlinePosition] =
        TerminalMetricModifiers::Absolute{.pixels = 10'000};
    positioned.typography.metricModifiers[
        TerminalMetric::UnderlineThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 1};
    const TerminalCellMetrics positionedMetrics =
        terminalCellMetrics(positioned.typography, dpr);
    const qint64 width = physical(positionedMetrics.cellWidth);
    const qint64 height = physical(positionedMetrics.cellHeight);
    const qint64 padding = height / 4;
    const qint64 canvasBottom = height + padding;
    const qint64 position =
        physical(positionedMetrics.underlinePosition);
    const qint64 thickness =
        physical(positionedMetrics.underlineThickness);
    const QRectF cell(
        0.0, 0.0,
        positionedMetrics.cellWidth,
        positionedMetrics.cellHeight);
    const QRectF cellCanvas = expectedSpriteCanvas(cell, dpr);
    const qint64 singleY = std::min(
        position, std::max<qint64>(0, canvasBottom - thickness));

    const TerminalPaneRenderProbeSnapshot single = render(
        positioned, TerminalUnderlineStyle::Single);
    QCOMPARE(single.underlineRects.size(), 1);
    QVERIFY(rectanglesFitInside(single.underlineRects, cellCanvas));
    QVERIFY(approximatelySame(
        single.underlineRects.constFirst().top(),
        static_cast<qreal>(singleY) / dpr));

    const TerminalPaneRenderProbeSnapshot dashed = render(
        positioned, TerminalUnderlineStyle::Dashed);
    QVERIFY(!dashed.underlineRects.isEmpty());
    QVERIFY(rectanglesFitInside(dashed.underlineRects, cellCanvas));
    QVector<QRectF> expectedDashes;
    const qint64 dashWidth = width / 3 + 1;
    const qint64 dashCount = width / dashWidth + 1;
    for (qint64 index = 0; index < dashCount; index += 2) {
        const QRectF dash(
            static_cast<qreal>(index * dashWidth) / dpr,
            static_cast<qreal>(singleY) / dpr,
            static_cast<qreal>(dashWidth) / dpr,
            static_cast<qreal>(thickness) / dpr);
        const QRectF clipped = dash.intersected(cellCanvas);
        if (!clipped.isEmpty()) {
            expectedDashes.append(clipped);
        }
    }
    QCOMPARE(dashed.underlineRects, expectedDashes);

    const qint64 doubleMiddle = std::min(
        position,
        std::max<qint64>(0, canvasBottom - 2 * thickness));
    const qint64 doubleFirst =
        std::max<qint64>(0, doubleMiddle - thickness);
    const qint64 doubleSecond = doubleMiddle + thickness;
    const TerminalPaneRenderProbeSnapshot doubleUnderline = render(
        positioned, TerminalUnderlineStyle::Double);
    QCOMPARE(doubleUnderline.underlineRects.size(), 2);
    QVERIFY(rectanglesFitInside(
        doubleUnderline.underlineRects, cellCanvas));
    QVERIFY(approximatelySame(
        doubleUnderline.underlineRects.at(0).top(),
        static_cast<qreal>(doubleFirst) / dpr));
    QVERIFY(approximatelySame(
        doubleUnderline.underlineRects.at(1).top(),
        static_cast<qreal>(doubleSecond) / dpr));

    const double radius =
        std::numbers::sqrt2 / 2.0 * static_cast<double>(thickness);
    const double dottedCenter = std::min(
        static_cast<double>(position)
            + 0.5 * static_cast<double>(thickness),
        static_cast<double>(canvasBottom) - std::ceil(radius));
    const qreal dottedTop = std::max(
        cellCanvas.top(),
        static_cast<qreal>(dottedCenter - radius) / dpr);
    const TerminalPaneRenderProbeSnapshot dotted = render(
        positioned, TerminalUnderlineStyle::Dotted);
    QVERIFY(!dotted.underlineRects.isEmpty());
    QVERIFY(rectanglesFitInside(dotted.underlineRects, cellCanvas));
    QVERIFY(rectanglesArePairwiseDisjoint(dotted.underlineRects));
    for (const QRectF &rect : dotted.underlineRects) {
        QVERIFY(approximatelySame(rect.height(), 1.0 / dpr));
    }
    const auto firstDotted = std::ranges::min_element(
        dotted.underlineRects, {}, &QRectF::top);
    const auto lastDotted = std::ranges::max_element(
        dotted.underlineRects, {}, &QRectF::bottom);
    QVERIFY(firstDotted != dotted.underlineRects.cend());
    QVERIFY(lastDotted != dotted.underlineRects.cend());
    QVERIFY(firstDotted->top() >= dottedTop - 1e-6);
    QVERIFY(firstDotted->top() <= dottedTop + 1.0 / dpr + 1e-6);
    QVERIFY(lastDotted->bottom()
            <= std::min(
                cellCanvas.bottom(),
                static_cast<qreal>(dottedCenter + radius) / dpr)
                + 1.0 / dpr + 1e-6);

    const double amplitude =
        static_cast<double>(width) / std::numbers::pi;
    const double curlyTopPhysical = std::min(
        static_cast<double>(position),
        static_cast<double>(canvasBottom)
            - amplitude - static_cast<double>(thickness));
    const qreal curlyTop = std::max(
        cellCanvas.top(),
        static_cast<qreal>(
            curlyTopPhysical
            - static_cast<double>(thickness) / 2.0) / dpr);
    const TerminalPaneRenderProbeSnapshot curly = render(
        positioned, TerminalUnderlineStyle::Curly);
    QVERIFY(!curly.underlineRects.isEmpty());
    QVERIFY(rectanglesFitInside(curly.underlineRects, cellCanvas));
    QVERIFY(rectanglesArePairwiseDisjoint(curly.underlineRects));
    QCOMPARE(curly.underlineRects.size(), width);
    for (const QRectF &rect : curly.underlineRects) {
        QVERIFY(approximatelySame(rect.width(), 1.0 / dpr));
    }
    const auto firstCurly = std::ranges::min_element(
        curly.underlineRects, {}, &QRectF::top);
    QVERIFY(firstCurly != curly.underlineRects.cend());
    QVERIFY(firstCurly->top() >= curlyTop - 1e-6);
    QVERIFY(firstCurly->top() <= curlyTop + 1.0 / dpr + 1e-6);

    LaunchOptions thickerCurve = positioned;
    thickerCurve.typography.metricModifiers[
        TerminalMetric::UnderlineThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 3};
    const TerminalPaneRenderProbeSnapshot thickCurly = render(
        thickerCurve, TerminalUnderlineStyle::Curly);
    QCOMPARE(thickCurly.underlineRects.size(), width);
    for (qsizetype column = 0;
         column < curly.underlineRects.size(); ++column) {
        QCOMPARE(
            thickCurly.underlineRects.at(column).left(),
            curly.underlineRects.at(column).left());
        QCOMPARE(
            thickCurly.underlineRects.at(column).width(),
            curly.underlineRects.at(column).width());
    }

    // Very large thicknesses exercise Canvas.rect's implicit clipping in
    // upstream Ghostty. The Qt geometry path must produce the same bounded
    // result instead of painting through arbitrary neighboring rows.
    LaunchOptions extreme = positioned;
    extreme.typography.metricModifiers[
        TerminalMetric::UnderlineThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 10'000};
    extreme.typography.metricModifiers[
        TerminalMetric::StrikethroughPosition] =
        TerminalMetricModifiers::Absolute{.pixels = -10'000};
    extreme.typography.metricModifiers[
        TerminalMetric::StrikethroughThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 10'000};
    extreme.typography.metricModifiers[
        TerminalMetric::OverlinePosition] =
        TerminalMetricModifiers::Absolute{.pixels = -10'000};
    extreme.typography.metricModifiers[
        TerminalMetric::OverlineThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 10'000};
    const TerminalCellMetrics extremeMetrics =
        terminalCellMetrics(extreme.typography, dpr);
    const QRectF extremeCell(
        0.0, 0.0,
        extremeMetrics.cellWidth, extremeMetrics.cellHeight);
    const QRectF extremeCanvas =
        expectedSpriteCanvas(extremeCell, dpr);
    for (const TerminalUnderlineStyle style : {
             TerminalUnderlineStyle::Double,
             TerminalUnderlineStyle::Dotted,
             TerminalUnderlineStyle::Curly,
             TerminalUnderlineStyle::Dashed,
        }) {
        const TerminalPaneRenderProbeSnapshot snapshot =
            render(extreme, style, true, true);
        // Ghostty's curly cap subtracts the full stroke width. At absurdly
        // large widths the entire stroked path can therefore sit above the
        // canvas; the other raster styles retain a visible clipped portion.
        if (style != TerminalUnderlineStyle::Curly) {
            QVERIFY(!snapshot.underlineRects.isEmpty());
        }
        QVERIFY(rectanglesFitInside(
            snapshot.underlineRects, extremeCanvas));
        QCOMPARE(snapshot.strikethroughRects.size(), 1);
        QVERIFY(rectanglesFitInside(
            snapshot.strikethroughRects, extremeCanvas));
        QCOMPARE(snapshot.overlineRects.size(), 1);
        QVERIFY(rectanglesFitInside(
            snapshot.overlineRects, extremeCanvas));
    }

    // A huge positive position starts entirely below the padded canvas and
    // therefore emits no geometry even when its thickness is also huge.
    LaunchOptions farBelow = extreme;
    farBelow.typography.metricModifiers[
        TerminalMetric::StrikethroughPosition] =
        TerminalMetricModifiers::Absolute{.pixels = 20'000};
    farBelow.typography.metricModifiers[
        TerminalMetric::OverlinePosition] =
        TerminalMetricModifiers::Absolute{.pixels = 20'000};
    const TerminalPaneRenderProbeSnapshot below = render(
        farBelow, TerminalUnderlineStyle::None, true, true);
    QVERIFY(below.strikethroughRects.isEmpty());
    QVERIFY(below.overlineRects.isEmpty());

    LaunchOptions thickCursor = options;
    thickCursor.typography.metricModifiers[
        TerminalMetric::UnderlinePosition] =
        TerminalMetricModifiers::Absolute{.pixels = 10'000};
    thickCursor.typography.metricModifiers[
        TerminalMetric::CursorThickness] =
        TerminalMetricModifiers::Absolute{.pixels = 10'000};
    thickCursor.typography.metricModifiers[
        TerminalMetric::CursorHeight] =
        TerminalMetricModifiers::Absolute{.pixels = 7};
    const TerminalCellMetrics cursorMetrics =
        terminalCellMetrics(thickCursor.typography, dpr);
    const QRectF cursorOuter(
        0.0, cursorMetrics.cursorTop,
        cursorMetrics.cellWidth, cursorMetrics.cursorHeight);
    const QRectF cursorCanvas =
        expectedSpriteCanvas(cursorOuter, dpr);
    const QRectF cursorCell(
        0.0, 0.0,
        cursorMetrics.cellWidth, cursorMetrics.cellHeight);
    const QRectF underlineCursorCanvas =
        expectedSpriteCanvas(cursorCell, dpr);

    const TerminalPaneRenderProbeSnapshot bar = render(
        thickCursor, TerminalUnderlineStyle::None,
        false, false, 0);
    QCOMPARE(bar.cursorRects.size(), 1);
    QVERIFY(rectanglesFitInside(bar.cursorRects, cursorCanvas));
    QCOMPARE(
        bar.cursorRects.constFirst(),
        QRectF(
            cursorMetrics.cursorBarLeft, cursorMetrics.cursorTop,
            cursorMetrics.cursorThickness,
            cursorMetrics.cursorHeight)
            .intersected(cursorCanvas));

    const qreal cursorUnderlineY = std::min(
        cursorMetrics.underlinePosition,
        cursorMetrics.underlineMaximumPosition);
    const TerminalPaneRenderProbeSnapshot underlineCursor = render(
        thickCursor, TerminalUnderlineStyle::None,
        false, false, 2);
    QCOMPARE(underlineCursor.cursorRects.size(), 1);
    QVERIFY(rectanglesFitInside(
        underlineCursor.cursorRects, underlineCursorCanvas));
    QCOMPARE(
        underlineCursor.cursorRects.constFirst(),
        QRectF(
            0.0, cursorUnderlineY,
            cursorMetrics.cellWidth,
            cursorMetrics.cursorThickness)
            .intersected(underlineCursorCanvas));

    const TerminalPaneRenderProbeSnapshot hollow = render(
        thickCursor, TerminalUnderlineStyle::None,
        false, false, 3);
    QCOMPARE(hollow.cursorRects, QVector<QRectF>({cursorOuter}));
    QVERIFY(rectanglesArePairwiseDisjoint(hollow.cursorRects));
    QVERIFY(rectanglesFitInside(hollow.cursorRects, cursorCanvas));
    QCOMPARE(
        totalRectangleArea(hollow.cursorRects),
        rectangleArea(cursorOuter));

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

void TerminalPaneTest::compiledProgramAvailabilityControlsEmergencyShortcuts()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    const GhosttyKeybindProgram availableEmpty =
        GhosttyKeybindProgram::compile(
            GhosttyKeybindSource::structured({})).program;
    const GhosttyKeybindProgram unavailable =
        GhosttyKeybindProgram::compile(GhosttyKeybindSource{}).program;
    QVERIFY(availableEmpty.isAvailable());
    QVERIFY(availableEmpty.isEmpty());
    QVERIFY(!unavailable.isAvailable());
    QVERIFY(unavailable.isEmpty());

    // The injected compiled generation is authoritative even when the launch
    // snapshot says the opposite. An available-but-empty program represents
    // an explicit empty Ghostty set, so emergency shortcuts stay disabled.
    TerminalPane configured(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, availableEmpty);
    QVERIFY(configured.keybindProgram().isSameGeneration(availableEmpty));
    QSignalSpy configuredNewTab(&configured, &TerminalPane::requestNewTab);
    auto *configuredController =
        configured.findChild<TerminalController *>();
    QVERIFY(configuredController != nullptr);
    QSignalSpy configuredForwarded(
        configuredController, &TerminalController::keyRequested);

    QKeyEvent configuredShortcut(
        QEvent::KeyPress, Qt::Key_T,
        Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("T"));
    QCoreApplication::sendEvent(&configured, &configuredShortcut);
    QCOMPARE(configuredNewTab.count(), 0);
    QCOMPARE(configuredForwarded.count(), 1);

    LaunchOptions misleadingOptions = options;
    misleadingOptions.keybindSource =
        GhosttyKeybindSource::structured({});
    TerminalPane fallback(
        misleadingOptions, nullptr, std::nullopt,
        TerminalSessionStartMode::Immediate, {}, unavailable);
    QVERIFY(fallback.keybindProgram().isSameGeneration(unavailable));
    QSignalSpy fallbackNewTab(&fallback, &TerminalPane::requestNewTab);
    auto *fallbackController = fallback.findChild<TerminalController *>();
    QVERIFY(fallbackController != nullptr);
    QSignalSpy fallbackForwarded(
        fallbackController, &TerminalController::keyRequested);

    QKeyEvent fallbackShortcut(
        QEvent::KeyPress, Qt::Key_T,
        Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("T"));
    QCoreApplication::sendEvent(&fallback, &fallbackShortcut);
    QCOMPARE(fallbackNewTab.count(), 1);
    QCOMPARE(fallbackForwarded.count(), 0);
}

void TerminalPaneTest::forwardsConsumedShiftForLayoutText()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured({});

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    auto *const controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    constexpr quint32 semicolonScanCode = KEY_SEMICOLON + 8U;
    QKeyEvent colonPress(QEvent::KeyPress, Qt::Key_Colon, Qt::ShiftModifier,
                         semicolonScanCode, 0, 0, QStringLiteral(":"));
    QCoreApplication::sendEvent(&pane, &colonPress);
    QCOMPARE(forwarded.count(), 1);
    TerminalKeyInput input =
        qvariant_cast<TerminalKeyInput>(forwarded.constLast().constFirst());
    QCOMPARE(input.text, QStringLiteral(":"));
    QCOMPARE(input.unshiftedCodepoint, uint32_t{';'});
    QVERIFY(input.modifiers & Qt::ShiftModifier);
    QVERIFY(input.consumedModifiers & Qt::ShiftModifier);

    QKeyEvent colonRelease(QEvent::KeyRelease, Qt::Key_Colon, Qt::ShiftModifier,
                           semicolonScanCode, 0, 0, QStringLiteral(":"));
    QCoreApplication::sendEvent(&pane, &colonRelease);
    QCOMPARE(forwarded.count(), 2);
    input = qvariant_cast<TerminalKeyInput>(forwarded.constLast().constFirst());
    QVERIFY(!input.pressed);
    QVERIFY(input.consumedModifiers & Qt::ShiftModifier);

    // Qt platform backends may expose either the produced punctuation key or
    // its level-zero key while retaining the same text and physical scan code.
    QKeyEvent semicolonPress(QEvent::KeyPress, Qt::Key_Semicolon,
                             Qt::ShiftModifier, semicolonScanCode, 0, 0,
                             QStringLiteral(":"));
    QCoreApplication::sendEvent(&pane, &semicolonPress);
    QCOMPARE(forwarded.count(), 3);
    input = qvariant_cast<TerminalKeyInput>(forwarded.constLast().constFirst());
    QCOMPARE(input.unshiftedCodepoint, uint32_t{';'});
    QVERIFY(input.consumedModifiers & Qt::ShiftModifier);

    // Shift remains effective when it does not transform printable text, and
    // for non-text navigation keys.
    QKeyEvent spacePress(QEvent::KeyPress, Qt::Key_Space, Qt::ShiftModifier,
                         QStringLiteral(" "));
    QCoreApplication::sendEvent(&pane, &spacePress);
    QCOMPARE(forwarded.count(), 4);
    input = qvariant_cast<TerminalKeyInput>(forwarded.constLast().constFirst());
    QCOMPARE(input.consumedModifiers, 0);

    QKeyEvent leftPress(QEvent::KeyPress, Qt::Key_Left, Qt::ShiftModifier);
    QCoreApplication::sendEvent(&pane, &leftPress);
    QCOMPARE(forwarded.count(), 5);
    input = qvariant_cast<TerminalKeyInput>(forwarded.constLast().constFirst());
    QCOMPARE(input.consumedModifiers, 0);
}

void TerminalPaneTest::forwardsAuthoritativeLayoutMetadata()
{
    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured({});

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    auto *const controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    QKeyEvent event(QEvent::KeyPress, Qt::Key_Y, Qt::NoModifier, KEY_Y + 8U, 0,
                    0, QStringLiteral("z"));
    const KeyboardLayoutTranslation layout{
        .unshiftedCodepoint = 'z',
        .consumedModifiers = Qt::GroupSwitchModifier,
        .capsLock = true,
        .numLock = true,
        .consumedCapsLock = true,
        .authoritative = true,
    };
    const ScopedKeyboardLayoutTranslation scope(event, layout);
    QCoreApplication::sendEvent(&pane, &event);

    QCOMPARE(forwarded.count(), 1);
    const TerminalKeyInput input =
        qvariant_cast<TerminalKeyInput>(forwarded.constFirst().constFirst());
    QCOMPARE(input.nativeScanCode, quint32{KEY_Y + 8U});
    QCOMPARE(input.text, QStringLiteral("z"));
    QCOMPARE(input.unshiftedCodepoint, std::uint32_t{'z'});
    QCOMPARE(input.consumedModifiers,
             static_cast<int>(Qt::GroupSwitchModifier));
    QVERIFY(input.capsLock);
    QVERIFY(input.numLock);
    QVERIFY(input.consumedCapsLock);

    GhosttyKeybindConfig keybinds;
    keybinds.root = {
        generationTestBinding({generationTestKey('&', GhosttyKeybindCtrl)},
                              QStringLiteral("new_tab"))};
    LaunchOptions boundOptions = options;
    boundOptions.keybindSource =
        GhosttyKeybindSource::structured(std::move(keybinds));
    TerminalPane boundPane(boundOptions, nullptr, std::nullopt,
                           TerminalSessionStartMode::Deferred);
    auto *const boundController = boundPane.findChild<TerminalController *>();
    QVERIFY(boundController != nullptr);
    QSignalSpy boundForwarded(boundController,
                              &TerminalController::keyRequested);
    QSignalSpy newTabs(&boundPane, &TerminalPane::requestNewTab);

    QKeyEvent bindingEvent(QEvent::KeyPress, Qt::Key_Ampersand,
                           Qt::ControlModifier, KEY_1 + 8U, 0, 0,
                           QString(QChar(0x1f)));
    const KeyboardLayoutTranslation frenchNumberRow{
        .unshiftedCodepoint = '&',
        .authoritative = true,
    };
    const ScopedKeyboardLayoutTranslation bindingScope(bindingEvent,
                                                       frenchNumberRow);
    QCoreApplication::sendEvent(&boundPane, &bindingEvent);
    QCOMPARE(newTabs.count(), 1);
    QCOMPARE(boundForwarded.count(), 0);
}

void TerminalPaneTest::remapsSidedModifiersAcrossBindingsAndTerminalInput()
{
    constexpr auto xkbKeycode = [](std::uint32_t evdevCode) {
        return static_cast<quint32>(evdevCode + 8U);
    };

    LaunchOptions options;
    options.workingDirectory = QDir::currentPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    GhosttyKeybindConfig keybinds;
    keybinds.root = {
        generationTestBinding({generationTestKey('n', GhosttyKeybindAlt)},
                              QStringLiteral("new_tab")),
    };
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(keybinds));
    options.modifierRemaps = {{
        .from =
            {
                .key = ModifierKey::Ctrl,
                .side = ModifierSide::Right,
            },
        .to =
            {
                .key = ModifierKey::Alt,
                .side = ModifierSide::Left,
            },
    }};

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Deferred);
    auto *const controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy newTabs(&pane, &TerminalPane::requestNewTab);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    const auto sendRightControl = [&pane, xkbKeycode](QEvent::Type type,
                                                      bool pressed) {
        QKeyEvent event(type, Qt::Key_Control,
                        pressed ? Qt::NoModifier : Qt::ControlModifier,
                        xkbKeycode(KEY_RIGHTCTRL), 0, 0);
        QCoreApplication::sendEvent(&pane, &event);
    };
    const auto sendKey = [&pane](QEvent::Type type, int key,
                                 Qt::KeyboardModifiers modifiers,
                                 QString text = {}) {
        QKeyEvent event(type, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };

    sendRightControl(QEvent::KeyPress, true);
    const int beforeBinding = forwarded.count();
    sendKey(QEvent::KeyPress, Qt::Key_N, Qt::ControlModifier,
            QStringLiteral("n"));
    QCOMPARE(newTabs.count(), 1);
    QCOMPARE(forwarded.count(), beforeBinding);
    sendKey(QEvent::KeyRelease, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(forwarded.count(), beforeBinding);
    sendRightControl(QEvent::KeyRelease, false);

    // The same preprocessed modifiers reach PTY encoding when no configured
    // binding consumes the key; native identity and text remain unchanged.
    sendRightControl(QEvent::KeyPress, true);
    const int beforeTerminalKey = forwarded.count();
    sendKey(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier,
            QString(QChar(0x18)));
    QCOMPARE(forwarded.count(), beforeTerminalKey + 1);
    const TerminalKeyInput terminalInput =
        qvariant_cast<TerminalKeyInput>(forwarded.constLast().constFirst());
    QCOMPARE(terminalInput.key, static_cast<int>(Qt::Key_X));
    QCOMPARE(terminalInput.text, QString(QChar(0x18)));
    QVERIFY(terminalInput.modifiers & Qt::AltModifier);
    QVERIFY(!(terminalInput.modifiers & Qt::ControlModifier));

    // Live replacement is part of the pane's matcher transaction and resets
    // the observed physical side rather than leaking it into a new policy.
    LaunchOptions reloaded = options;
    reloaded.modifierRemaps.clear();
    const GhosttyKeybindProgram generation = pane.keybindProgram();
    pane.applyRuntimeOptions(reloaded, generation);
    const int beforeCleared = forwarded.count();
    sendKey(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier,
            QString(QChar(0x18)));
    QCOMPARE(forwarded.count(), beforeCleared + 1);
    const TerminalKeyInput clearedInput =
        qvariant_cast<TerminalKeyInput>(forwarded.constLast().constFirst());
    QVERIFY(clearedInput.modifiers & Qt::ControlModifier);
    QVERIFY(!(clearedInput.modifiers & Qt::AltModifier));
}

void TerminalPaneTest::routesConfiguredBindingsAndDisablesEmergencyFallback()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("alt+n=new_tab"),
        QStringLiteral("ctrl+x=increase_font_size:2.5"),
        QStringLiteral("ctrl+enter=toggle_fullscreen"),
        QStringLiteral("alt+m=toggle_maximize"),
        QStringLiteral("alt+d=toggle_window_decorations"),
        QStringLiteral("ctrl+w=close_tab:this"),
        QStringLiteral("ctrl+shift+w=close_surface"),
        QStringLiteral("ctrl+r=reload_config"),
        QStringLiteral("alt+f4=close_window"),
        QStringLiteral("ctrl+y=open_config"),
        QStringLiteral("alt+a=close_all_windows"),
        QStringLiteral("unconsumed:alt+b=close_all_windows"),
        QStringLiteral("performable:alt+c=close_all_windows"),
        QStringLiteral("alt+z=close_all_windows"),
        QStringLiteral("chain=new_tab"),
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
    });

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
    QSignalSpy copied(
        controller, &TerminalController::copyActionRequested);
    QSignalSpy terminalActions(
        controller, &TerminalController::terminalActionReady);
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

    const int beforeDeprecatedNoOp = forwarded.count();
    QKeyEvent deprecatedNoOp(QEvent::KeyPress, Qt::Key_A, Qt::AltModifier,
                             QStringLiteral("a"));
    QCoreApplication::sendEvent(&pane, &deprecatedNoOp);
    QCOMPARE(
        applicationActionCount(ApplicationAction::DeprecatedCloseAllWindows),
        1);
    QCOMPARE(forwarded.count(), beforeDeprecatedNoOp);

    QKeyEvent unconsumedDeprecatedNoOp(QEvent::KeyPress, Qt::Key_B,
                                       Qt::AltModifier, QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &unconsumedDeprecatedNoOp);
    QCOMPARE(
        applicationActionCount(ApplicationAction::DeprecatedCloseAllWindows),
        2);
    QCOMPARE(forwarded.count(), beforeDeprecatedNoOp + 1);

    QKeyEvent performableDeprecatedNoOp(QEvent::KeyPress, Qt::Key_C,
                                        Qt::AltModifier, QStringLiteral("c"));
    QCoreApplication::sendEvent(&pane, &performableDeprecatedNoOp);
    QCOMPARE(
        applicationActionCount(ApplicationAction::DeprecatedCloseAllWindows),
        3);
    QCOMPARE(forwarded.count(), beforeDeprecatedNoOp + 1);

    QKeyEvent chainedDeprecatedNoOp(QEvent::KeyPress, Qt::Key_Z,
                                    Qt::AltModifier, QStringLiteral("z"));
    QCoreApplication::sendEvent(&pane, &chainedDeprecatedNoOp);
    QCOMPARE(
        applicationActionCount(ApplicationAction::DeprecatedCloseAllWindows),
        4);
    QCOMPARE(newTab.count(), 2);
    QCOMPARE(forwarded.count(), beforeDeprecatedNoOp + 1);

    // A normal consumed binding suppresses terminal input even when the
    // worker authoritatively reports that there is nothing to copy.
    QKeyEvent consumedEmptyCopy(QEvent::KeyPress, Qt::Key_B,
                                Qt::ControlModifier, QString(QChar(0x02)));
    QCoreApplication::sendEvent(&pane, &consumedEmptyCopy);
    QCOMPARE(copied.count(), 1);
    QTRY_COMPARE(terminalActions.count(), 1);
    QCOMPARE(forwarded.count(), beforeDeprecatedNoOp + 1);

    // Direct programmatic dispatch reports that the correlated request was
    // accepted; its eventual performed state is delivered asynchronously.
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("copy_to_clipboard")));
    QCOMPARE(copied.count(), 2);
    QTRY_COMPARE(terminalActions.count(), 2);

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
    QCOMPARE(newTab.count(), 3);

    // Ghostty executes the complete chain before reporting a surface-closing
    // outcome. Workspace removal is deferred, so the later action is safe.
    QKeyEvent closingChain(QEvent::KeyPress, Qt::Key_K,
                           Qt::ControlModifier, QString(QChar(0x0b)));
    QCoreApplication::sendEvent(&pane, &closingChain);
    QCOMPARE(closeTab.count(), 2);
    QCOMPARE(qvariant_cast<CloseTabMode>(
                 closeTab.constLast().constFirst()),
             CloseTabMode::Right);
    QCOMPARE(newTab.count(), 4);

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

    // A performable copy waits for the worker's selection decision, then an
    // unavailable result falls through to terminal input.
    const int beforeEmptyCopy = forwarded.count();
    QKeyEvent emptyCopy(QEvent::KeyPress, Qt::Key_C,
                        Qt::ControlModifier, QString(QChar(0x03)));
    QCoreApplication::sendEvent(&pane, &emptyCopy);
    QCOMPARE(copied.count(), 3);
    QTRY_COMPARE(terminalActions.count(), 3);
    QTRY_COMPARE(forwarded.count(), beforeEmptyCopy + 1);

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

    // The title action follows the same performable contract: the exact direct
    // argv[0] launch base and later non-empty bases consume the key, while an
    // explicit-empty raw title passes it through.
    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const int beforeTitleCopy = forwarded.count();
    clipboard->setText(QStringLiteral("launch title sentinel"),
                       QClipboard::Clipboard);
    QKeyEvent emptyTitleCopy(QEvent::KeyPress, Qt::Key_D,
                             Qt::ControlModifier, QString(QChar(0x04)));
    QCoreApplication::sendEvent(&pane, &emptyTitleCopy);
    QCOMPARE(forwarded.count(), beforeTitleCopy);
    QTRY_COMPARE(clipboard->text(QClipboard::Clipboard),
                 QStringLiteral("/bin/true"));

    pane.setSurfaceTitle(QString{});
    QKeyEvent explicitEmptyTitleCopy(
        QEvent::KeyPress, Qt::Key_D,
        Qt::ControlModifier, QString(QChar(0x04)));
    QCoreApplication::sendEvent(&pane, &explicitEmptyTitleCopy);
    QCOMPARE(forwarded.count(), beforeTitleCopy + 1);

    const QString performableTitle = QStringLiteral("performable title");
    pane.setSurfaceTitle(performableTitle);
    clipboard->setText(QStringLiteral("performable sentinel"),
                       QClipboard::Clipboard);
    QKeyEvent titleCopy(QEvent::KeyPress, Qt::Key_D,
                        Qt::ControlModifier, QString(QChar(0x04)));
    QCoreApplication::sendEvent(&pane, &titleCopy);
    QCOMPARE(forwarded.count(), beforeTitleCopy + 1);
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

    const int beforeDecorations = forwarded.count();
    QKeyEvent decorationsPress(QEvent::KeyPress, Qt::Key_D, Qt::AltModifier,
                               QStringLiteral("d"));
    QCoreApplication::sendEvent(&pane, &decorationsPress);
    QCOMPARE(workspaceRequests.size(), 4);
    QCOMPARE(workspaceRequests.constLast().action,
             WorkspaceAction::ToggleWindowDecorations);
    QCOMPARE(forwarded.count(), beforeDecorations);
    QKeyEvent decorationsRelease(QEvent::KeyRelease, Qt::Key_D,
                                 Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &decorationsRelease);
    QCOMPARE(forwarded.count(), beforeDecorations);
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("toggle_window_decorations:")));
    QCOMPARE(workspaceRequests.size(), 4);

    const int beforeUnavailableNavigation = forwarded.count();
    QKeyEvent unavailableNavigation(QEvent::KeyPress, Qt::Key_G,
                                    Qt::ControlModifier, QString(QChar(0x07)));
    QCoreApplication::sendEvent(&pane, &unavailableNavigation);
    QCOMPARE(forwarded.count(), beforeUnavailableNavigation + 1);
}

void TerminalPaneTest::routesBroadConfiguredActionEffects()
{
    qRegisterMetaType<GhosttyCompiledActionChain>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    const auto controlKey = [](quint32 codepoint) {
        return GhosttyKeybindTrigger{
            .kind = GhosttyKeybindKeyKind::Unicode,
            .unicodeCodepoint = codepoint,
            .modifiers = GhosttyKeybindCtrl,
        };
    };
    GhosttyKeybindConfig config;
    config.root = {
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
    GhosttyKeybindConfig reloadedConfig = config;
    reloadedConfig.root.append(GhosttyKeybindDefinition{
        .sequence = {controlKey('x')},
        .actions = {QStringLiteral("new_tab")},
    });
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    // The match owns its compiled chain. Replacing the pane's keybinding trie
    // from an earlier direct signal observer must not invalidate the argument
    // seen by later observers or the remainder of the key event.
    LaunchOptions reloaded = options;
    reloaded.keybindSource =
        GhosttyKeybindSource::structured(std::move(reloadedConfig));
    bool reloadedDuringBroadDispatch = false;
    connect(&pane, &TerminalPane::broadActionsRequested, &pane,
            [&](const GhosttyCompiledActionChain &) {
                if (reloadedDuringBroadDispatch) return;
                reloadedDuringBroadDispatch = true;
                pane.applyRuntimeOptions(reloaded);
            });
    QSignalSpy broadActions(&pane, &TerminalPane::broadActionsRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy copied(controller, &TerminalController::copyRequested);

    // A broad closing action takes precedence over a later ignore action, so
    // the release remains suppressed even though ignore is press-only alone.
    QKeyEvent closePress(QEvent::KeyPress, Qt::Key_U,
                         Qt::ControlModifier, QString(QChar(0x15)));
    QCoreApplication::sendEvent(&pane, &closePress);
    QVERIFY(reloadedDuringBroadDispatch);
    QCOMPARE(broadActions.count(), 1);
    const GhosttyCompiledActionChain closeChain =
        qvariant_cast<GhosttyCompiledActionChain>(
            broadActions.constFirst().constFirst());
    QCOMPARE(closeChain.serializedActions(),
             QStringList({QStringLiteral("close_tab:right"),
                          QStringLiteral("ignore")}));
    QCOMPARE(closeChain.inputEffect,
             GhosttyActionInputEffect::ClosingAction);
    QVERIFY(!closeChain.applicationOnly);
    QCOMPARE(closeChain.entries.size(), 2);
    QCOMPARE(closeChain.entries.at(0).scope,
             GhosttyActionScope::Surface);
    QVERIFY(closeChain.entries.at(0).action.has_value());
    const auto *closeRequest = std::get_if<WorkspaceActionRequest>(
        &*closeChain.entries.at(0).action);
    QVERIFY(closeRequest != nullptr);
    QCOMPARE(closeRequest->action, WorkspaceAction::CloseTab);
    QCOMPARE(closeRequest->context.closeTabMode, CloseTabMode::Right);
    QCOMPARE(closeChain.entries.at(1).scope,
             GhosttyActionScope::Application);
    QVERIFY(closeChain.entries.at(1).action.has_value());
    const auto *ignore = std::get_if<ApplicationAction>(
        &*closeChain.entries.at(1).action);
    QVERIFY(ignore != nullptr);
    QCOMPARE(*ignore, ApplicationAction::Ignore);
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
    const GhosttyCompiledActionChain copyChain =
        qvariant_cast<GhosttyCompiledActionChain>(
            broadActions.constLast().constFirst());
    QCOMPARE(copyChain.serializedActions(),
             QStringList({QStringLiteral("copy_to_clipboard")}));
    QCOMPARE(copyChain.inputEffect, GhosttyActionInputEffect::None);
    QVERIFY(!copyChain.applicationOnly);
    QCOMPARE(copyChain.entries.size(), 1);
    QVERIFY(copyChain.entries.constFirst().action.has_value());
    const auto *paneAction = std::get_if<GhosttyPaneAction>(
        &*copyChain.entries.constFirst().action);
    QVERIFY(paneAction != nullptr);
    QVERIFY(std::holds_alternative<
            GhosttyPaneActions::CopyToClipboard>(*paneAction));
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
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("performable:alt+u=scroll_to_selection"),
        QStringLiteral("performable:alt+i=adjust_selection:left"),
        QStringLiteral("alt+y=select_all"),
        QStringLiteral("chain=adjust_selection:right"),
        QStringLiteral("chain=scroll_to_selection"),
    });

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy scrolls(controller, &TerminalController::scrollRequested);
    QSignalSpy selectionScrolls(
        controller, &TerminalController::scrollToSelectionActionRequested);
    QSignalSpy selectAll(
        controller, &TerminalController::selectAllActionRequested);
    QSignalSpy copied(
        controller, &TerminalController::copyActionRequested);
    QSignalSpy selectionAdjustments(
        controller,
        &TerminalController::selectionAdjustmentActionRequested);
    QSignalSpy terminalActions(
        controller, &TerminalController::terminalActionReady);

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
    const int requestedRows =
        resizes.constLast().constFirst().value<TerminalSessionGeometry>().rows;
    QVERIFY(requestedRows > 0);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("scroll_page_down")));
    QCOMPARE(qvariant_cast<TerminalViewportRequest>(
                 scrolls.constFirst().constFirst()).delta,
             qint64(requestedRows));
    scrolls.clear();

    // Selection-dependent performable bindings always ask the worker. A
    // blank selection resolves as unavailable and only then reaches the PTY.
    QKeyEvent missingScrollSelection(
        QEvent::KeyPress, Qt::Key_U, Qt::AltModifier, QStringLiteral("u"));
    QCoreApplication::sendEvent(&pane, &missingScrollSelection);
    QCOMPARE(selectionScrolls.count(), 1);
    QTRY_COMPARE(terminalActions.count(), 1);
    QTRY_COMPARE(forwarded.count(), 1);
    QKeyEvent missingAdjustment(
        QEvent::KeyPress, Qt::Key_I, Qt::AltModifier, QStringLiteral("i"));
    QCoreApplication::sendEvent(&pane, &missingAdjustment);
    QCOMPARE(selectionAdjustments.count(), 1);
    QTRY_COMPARE(terminalActions.count(), 2);
    QTRY_COMPARE(forwarded.count(), 2);

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

    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("scroll_to_selection")));
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("adjust_selection:right")));
    QCOMPARE(scrolls.count(), beforeUnsafe);
    QCOMPARE(selectionScrolls.count(), 2);
    QCOMPARE(selectionAdjustments.count(), 2);
    QTRY_COMPARE(terminalActions.count(), 4);
    for (int index = 2; index < 4; ++index) {
        const TerminalActionResult result =
            qvariant_cast<TerminalActionResult>(
                terminalActions.at(index).constFirst());
        QCOMPARE(result.outcome, TerminalActionOutcome::Unavailable);
        QVERIFY(!result.performed);
    }

    selectionScrolls.clear();
    selectionAdjustments.clear();
    terminalActions.clear();

    // The chain waits for each correlated result. Select-all establishes the
    // worker selection before adjustment and scroll run, without consulting
    // the asynchronously updated GUI selection cache.
    QKeyEvent chainedSelection(
        QEvent::KeyPress, Qt::Key_Y, Qt::AltModifier, QStringLiteral("y"));
    QCoreApplication::sendEvent(&pane, &chainedSelection);
    QCOMPARE(selectAll.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(selectionAdjustments.count(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(selectionScrolls.count(), 1, 3000);
    QCOMPARE(qvariant_cast<TerminalSelectionAdjustment>(
                 selectionAdjustments.constFirst().at(1)),
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
    QSet<quint64> copyRequestIds;
    for (const QList<QVariant> &arguments : copied) {
        const quint64 requestId = arguments.constFirst().toULongLong();
        QVERIFY(requestId != 0);
        copyRequestIds.insert(requestId);
    }
    QCOMPARE(copyRequestIds.size(), copied.count());

    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("scroll_to_selection")));
    QCOMPARE(selectionScrolls.count(), 2);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("adjust_selection:right")));
    QCOMPARE(selectionAdjustments.count(), 2);
    QCOMPARE(qvariant_cast<TerminalSelectionAdjustment>(
                 selectionAdjustments.at(1).at(1)),
             TerminalSelectionAdjustment::Right);

    const int beforeBoundActions = forwarded.count();
    QKeyEvent availableScrollSelection(
        QEvent::KeyPress, Qt::Key_U, Qt::AltModifier, QStringLiteral("u"));
    QCoreApplication::sendEvent(&pane, &availableScrollSelection);
    QKeyEvent availableAdjustment(
        QEvent::KeyPress, Qt::Key_I, Qt::AltModifier, QStringLiteral("i"));
    QCoreApplication::sendEvent(&pane, &availableAdjustment);
    QTRY_COMPARE(selectionScrolls.count(), 3);
    QTRY_COMPARE(selectionAdjustments.count(), 3);
    QCOMPARE(forwarded.count(), beforeBoundActions);
}

void TerminalPaneTest::selectionActionPerformabilityUsesWorkerState_data()
{
    QTest::addColumn<QString>("action");

    QTest::newRow("copy")
        << QStringLiteral("copy_to_clipboard:plain");
    QTest::newRow("search")
        << QStringLiteral("search_selection");
    QTest::newRow("scroll")
        << QStringLiteral("scroll_to_selection");
    QTest::newRow("adjust")
        << QStringLiteral("adjust_selection:right");
}

void TerminalPaneTest::selectionActionPerformabilityUsesWorkerState()
{
    QFETCH(QString, action);
    qRegisterMetaType<TerminalActionResult>();

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"),
        QStringLiteral("-c"),
        QStringLiteral(
            "printf 'worker-authoritative-selection'; sleep 5"),
    };
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("performable:alt+x=%1").arg(action),
    });

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy completions(
        controller, &TerminalController::terminalActionReady);

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(
            updates, QStringLiteral("worker-authoritative-selection")),
        5000);
    QVERIFY(!controller->selectionAvailable());

    // Stale-false cache: select-all and the action retain FIFO order on the
    // worker even though the GUI has not received selection availability.
    controller->selectAll();
    QVERIFY(!controller->selectionAvailable());
    QKeyEvent selectedPress(
        QEvent::KeyPress, Qt::Key_X, Qt::AltModifier,
        QStringLiteral("x"));
    QCoreApplication::sendEvent(&pane, &selectedPress);
    QTRY_COMPARE_WITH_TIMEOUT(completions.count(), 1, 3000);
    const TerminalActionResult selectedResult =
        qvariant_cast<TerminalActionResult>(
            completions.constFirst().constFirst());
    QCOMPARE(selectedResult.outcome, TerminalActionOutcome::Success);
    QVERIFY(selectedResult.performed);
    QCOMPARE(forwarded.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(controller->selectionAvailable(), 3000);

    QKeyEvent selectedRelease(
        QEvent::KeyRelease, Qt::Key_X, Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &selectedRelease);
    QCoreApplication::processEvents();
    const int forwardedBeforeClear = forwarded.count();

    // Stale-true cache: the worker processes clear-selection first and
    // reports the immediately following action unavailable. Only after that
    // authoritative result does the performable binding replay to the PTY.
    controller->clearSelection();
    QVERIFY(controller->selectionAvailable());
    QKeyEvent clearedPress(
        QEvent::KeyPress, Qt::Key_X, Qt::AltModifier,
        QStringLiteral("x"));
    QCoreApplication::sendEvent(&pane, &clearedPress);
    QTRY_COMPARE_WITH_TIMEOUT(completions.count(), 2, 3000);
    const TerminalActionResult clearedResult =
        qvariant_cast<TerminalActionResult>(
            completions.at(1).constFirst());
    QCOMPARE(clearedResult.outcome, TerminalActionOutcome::Unavailable);
    QVERIFY(!clearedResult.performed);
    QTRY_COMPARE_WITH_TIMEOUT(
        forwarded.count(), forwardedBeforeClear + 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->selectionAvailable(), 3000);
}

void TerminalPaneTest::routesTerminalControlActions()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("alt+c=csi:31m"),
        QStringLiteral("chain=esc:7"),
        QStringLiteral(R"(chain=text:\\x00\\n\\u{1f47b})"),
        QStringLiteral("chain=reset"),
    });

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

void TerminalPaneTest::suspendsTerminalActionChainsUntilCorrelatedEffectsCommit()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("alt+b=select_all"),
        QStringLiteral("chain=adjust_selection:right"),
        QStringLiteral("chain=scroll_to_selection"),
        QStringLiteral("chain=search_selection"),
        QStringLiteral("chain=copy_to_clipboard:plain"),
        QStringLiteral("chain=write_screen_file:open"),
        QStringLiteral("chain=paste_from_clipboard"),
    });

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    QSignalSpy selectAll(
        controller, &TerminalController::selectAllActionRequested);
    QSignalSpy adjustments(
        controller,
        &TerminalController::selectionAdjustmentActionRequested);
    QSignalSpy selectionScrolls(
        controller, &TerminalController::scrollToSelectionActionRequested);
    QSignalSpy searchSelection(
        controller, &TerminalController::searchSelectionActionRequested);
    QSignalSpy copies(
        controller, &TerminalController::copyActionRequested);
    QSignalSpy files(
        controller, &TerminalController::writeTerminalFileRequested);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const QString sentinel = QStringLiteral("correlated-chain-sentinel");
    const QString copiedText = QStringLiteral("correlated selection");
    clipboard->setText(sentinel, QClipboard::Clipboard);
    const auto clipboardCleanup = qScopeGuard([clipboard] {
        clipboard->clear(QClipboard::Clipboard);
    });

    QStringList effectOrder;
    QString clipboardAtOpen;
    QString clipboardAtPaste;
    QList<QUrl> openedUrls;
    connect(
        &pane, &TerminalPane::searchUiFocusRequested, &pane,
        [&effectOrder] {
            effectOrder.append(QStringLiteral("search"));
        });
    pane.setUrlOpener(
        [&](const QUrl &url) {
            effectOrder.append(QStringLiteral("open"));
            clipboardAtOpen =
                clipboard->text(QClipboard::Clipboard);
            openedUrls.append(url);
            return true;
        });
    connect(
        controller, &TerminalController::pasteRequested, &pane,
        [&](const QString &) {
            effectOrder.append(QStringLiteral("paste"));
            clipboardAtPaste =
                clipboard->text(QClipboard::Clipboard);
        });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_B, Qt::AltModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &press);
    QCOMPARE(selectAll.count(), 1);
    QCOMPARE(adjustments.count(), 0);
    QCOMPARE(selectionScrolls.count(), 0);
    QCOMPARE(searchSelection.count(), 0);
    QCOMPARE(copies.count(), 0);
    QCOMPARE(files.count(), 0);
    QCOMPARE(pasted.count(), 0);
    QCOMPARE(openedUrls.size(), 0);
    QCOMPARE(clipboard->text(QClipboard::Clipboard), sentinel);
    QCOMPARE(forwarded.count(), 0);

    // The release arrives while the chain is worker-suspended. It must stay
    // deferred until the aggregate consumed outcome is known.
    QKeyEvent release(
        QEvent::KeyRelease, Qt::Key_B, Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &release);
    QCOMPARE(forwarded.count(), 0);

    const quint64 selectAllRequestId =
        selectAll.constFirst().constFirst().toULongLong();
    QVERIFY(selectAllRequestId != 0);
    Q_EMIT controller->terminalActionReady({
        .requestId = selectAllRequestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::None,
        .performed = true,
        .payload = {},
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    });
    QCoreApplication::processEvents();
    QCOMPARE(adjustments.count(), 1);
    QCOMPARE(selectionScrolls.count(), 0);
    QCOMPARE(searchSelection.count(), 0);
    QCOMPARE(copies.count(), 0);
    QCOMPARE(files.count(), 0);
    QCOMPARE(clipboard->text(QClipboard::Clipboard), sentinel);

    const quint64 adjustmentRequestId =
        adjustments.constFirst().constFirst().toULongLong();
    QVERIFY(adjustmentRequestId != 0);
    QCOMPARE(qvariant_cast<TerminalSelectionAdjustment>(
                 adjustments.constFirst().at(1)),
             TerminalSelectionAdjustment::Right);
    Q_EMIT controller->terminalActionReady({
        .requestId = adjustmentRequestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::None,
        .performed = true,
    });
    QCoreApplication::processEvents();
    QCOMPARE(selectionScrolls.count(), 1);
    QCOMPARE(searchSelection.count(), 0);
    QCOMPARE(copies.count(), 0);

    const quint64 scrollRequestId =
        selectionScrolls.constFirst().constFirst().toULongLong();
    QVERIFY(scrollRequestId != 0);
    Q_EMIT controller->terminalActionReady({
        .requestId = scrollRequestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::None,
        .performed = true,
    });
    QCoreApplication::processEvents();
    QCOMPARE(searchSelection.count(), 1);
    QCOMPARE(copies.count(), 0);

    const quint64 searchRequestId =
        searchSelection.constFirst().constFirst().toULongLong();
    QVERIFY(searchRequestId != 0);
    const QString selectionQuery =
        QStringLiteral("correlated selection query");
    Q_EMIT controller->terminalActionReady({
        .requestId = searchRequestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::StartSearch,
        .performed = true,
        .payload = selectionQuery,
    });
    QCOMPARE(copies.count(), 0);
    QVERIFY(!pane.searchUiActive());
    QCoreApplication::processEvents();
    QVERIFY(pane.searchUiActive());
    QCOMPARE(pane.searchUiText(), selectionQuery);
    QCOMPARE(effectOrder, QStringList({QStringLiteral("search")}));
    QCOMPARE(copies.count(), 1);

    const quint64 copyRequestId =
        copies.constFirst().constFirst().toULongLong();
    QVERIFY(copyRequestId != 0);
    QVERIFY(copyRequestId != selectAllRequestId);
    const quint64 unrelatedRequestId =
        copyRequestId == std::numeric_limits<quint64>::max()
        ? copyRequestId - 1 : copyRequestId + 1;
    Q_EMIT controller->terminalActionReady({
        .requestId = unrelatedRequestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::Clipboard,
        .performed = true,
        .payload = QStringLiteral("wrong result"),
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    });
    QCoreApplication::processEvents();
    QCOMPARE(files.count(), 0);
    QCOMPARE(clipboard->text(QClipboard::Clipboard), sentinel);

    Q_EMIT controller->terminalActionReady({
        .requestId = copyRequestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::Clipboard,
        .performed = true,
        .payload = copiedText,
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    });

    // The pane connection is queued: neither the GUI effect nor the later
    // action may overtake delivery of the correlated completion.
    QCOMPARE(clipboard->text(QClipboard::Clipboard), sentinel);
    QCOMPARE(files.count(), 0);
    QCoreApplication::processEvents();
    QCOMPARE(clipboard->text(QClipboard::Clipboard), copiedText);
    QCOMPARE(files.count(), 1);
    QCOMPARE(pasted.count(), 0);
    QCOMPARE(openedUrls.size(), 0);

    const QList<QVariant> fileArguments = files.constFirst();
    QCOMPARE(fileArguments.size(), 2);
    const quint64 fileRequestId =
        fileArguments.constFirst().toULongLong();
    QVERIFY(fileRequestId != 0);
    QVERIFY(fileRequestId != copyRequestId);
    const TerminalWriteFileAction fileAction =
        qvariant_cast<TerminalWriteFileAction>(
            fileArguments.at(1));
    QCOMPARE(fileAction.location, TerminalFileLocation::Screen);
    QCOMPARE(fileAction.disposition, TerminalFileDisposition::Open);
    QCOMPARE(fileAction.format, TerminalFileFormat::Plain);

    const QString artifactPath =
        QStringLiteral("/tmp/correlated-terminal-screen.txt");
    Q_EMIT controller->terminalActionReady({
        .requestId = fileRequestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::OpenFile,
        .performed = true,
        .payload = artifactPath,
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    });
    QCOMPARE(openedUrls.size(), 0);
    QCOMPARE(pasted.count(), 0);
    QCoreApplication::processEvents();

    QCOMPARE(
        openedUrls,
        QList<QUrl>{QUrl::fromLocalFile(artifactPath)});
    QCOMPARE(pasted.count(), 1);
    QCOMPARE(pasted.constFirst().constFirst().toString(), copiedText);
    QCOMPARE(
        effectOrder,
        QStringList({
            QStringLiteral("search"),
            QStringLiteral("open"),
            QStringLiteral("paste"),
        }));
    QCOMPARE(clipboardAtOpen, copiedText);
    QCOMPARE(clipboardAtPaste, copiedText);
    QCOMPARE(forwarded.count(), 0);
}

void TerminalPaneTest::cancelsPendingTerminalActionChainsBeforeSessionStart()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("alt+b=write_screen_file:copy"),
        QStringLiteral("chain=paste_from_clipboard"),
    });

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    QSignalSpy files(
        controller, &TerminalController::writeTerminalFileRequested);
    QSignalSpy completions(
        controller, &TerminalController::terminalActionReady);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const QString sentinel =
        QStringLiteral("cancelled-action-chain-clipboard");
    clipboard->setText(sentinel, QClipboard::Clipboard);
    const auto clipboardCleanup = qScopeGuard([clipboard] {
        clipboard->clear(QClipboard::Clipboard);
    });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_B, Qt::AltModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &press);
    QKeyEvent release(
        QEvent::KeyRelease, Qt::Key_B, Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &release);
    QCOMPARE(files.count(), 1);
    QCOMPARE(pasted.count(), 0);
    QCOMPARE(forwarded.count(), 0);

    const quint64 requestId =
        files.constFirst().constFirst().toULongLong();
    QVERIFY(requestId != 0);
    controller->beginShutdown();
    QCOMPARE(completions.count(), 1);
    const TerminalActionResult completion =
        qvariant_cast<TerminalActionResult>(
            completions.constFirst().constFirst());
    QCOMPARE(completion.requestId, requestId);
    QCOMPARE(completion.outcome, TerminalActionOutcome::Failed);
    QCOMPARE(completion.effect, TerminalActionEffect::None);
    QVERIFY(!completion.performed);

    QTRY_COMPARE(pasted.count(), 1);
    QCOMPARE(pasted.constFirst().constFirst().toString(), sentinel);
    QCOMPARE(forwarded.count(), 0);
}

void TerminalPaneTest::dropsQueuedTerminalEffectWhenShutdownBegins()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("sleep 5"),
    };
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("alt+b=write_screen_file:open"),
        QStringLiteral("chain=paste_from_clipboard"),
    });

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    // Keep the request pending while retaining a real started-worker shutdown
    // signal. The test supplies the correlated result explicitly.
    QVERIFY(QObject::disconnect(
        controller, &TerminalController::writeTerminalFileRequested,
        nullptr, nullptr));

    QSignalSpy files(
        controller, &TerminalController::writeTerminalFileRequested);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy inputMethods(
        controller, &TerminalController::inputMethodRequested);
    QStringList dispatchOrder;
    connect(controller, &TerminalController::shutdownRequested,
            this, [&dispatchOrder] {
                dispatchOrder.append(QStringLiteral("shutdown"));
            });
    connect(controller, &TerminalController::pasteRequested,
            this, [&dispatchOrder] {
                dispatchOrder.append(QStringLiteral("paste"));
            });
    connect(controller, &TerminalController::keyRequested,
            this, [&dispatchOrder] {
                dispatchOrder.append(QStringLiteral("key"));
            });
    connect(controller, &TerminalController::inputMethodRequested,
            this, [&dispatchOrder] {
                dispatchOrder.append(QStringLiteral("ime"));
            });
    int openCount = 0;
    pane.setUrlOpener([&openCount](const QUrl &) {
        ++openCount;
        return true;
    });

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const QString sentinel =
        QStringLiteral("shutdown-terminal-action-chain");
    clipboard->setText(sentinel, QClipboard::Clipboard);
    const auto clipboardCleanup = qScopeGuard([clipboard] {
        clipboard->clear(QClipboard::Clipboard);
    });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_B, Qt::AltModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &press);
    QKeyEvent release(
        QEvent::KeyRelease, Qt::Key_B, Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &release);
    QCOMPARE(files.count(), 1);
    QCOMPARE(pasted.count(), 0);
    QCOMPARE(forwarded.count(), 0);
    QCOMPARE(inputMethods.count(), 0);

    // Unrelated input joins the suspended pane FIFO. Shutdown must reach the
    // worker queue before resolving the barrier releases either item.
    QKeyEvent ordinaryPress(
        QEvent::KeyPress, Qt::Key_C, Qt::NoModifier,
        QStringLiteral("c"));
    QCoreApplication::sendEvent(&pane, &ordinaryPress);
    QInputMethodEvent deferredIme;
    deferredIme.setCommitString(QStringLiteral("deferred-ime"));
    QCoreApplication::sendEvent(&pane, &deferredIme);
    QCOMPARE(forwarded.count(), 0);
    QCOMPARE(inputMethods.count(), 0);

    const quint64 requestId =
        files.constFirst().constFirst().toULongLong();
    QVERIFY(requestId != 0);
    Q_EMIT controller->terminalActionReady(
        successfulOpenFileResult(
            requestId,
            QStringLiteral("/tmp/pre-shutdown-terminal-action.txt")));
    QCOMPARE(openCount, 0);
    QCOMPARE(pasted.count(), 0);

    // Shutdown invalidates both a completion still held by the worker and a
    // successful GUI-effect result already posted to the pane.
    pane.beginShutdown();
    QCOMPARE(openCount, 0);
    QCOMPARE(pasted.count(), 1);
    QCOMPARE(forwarded.count(), 1);
    QCOMPARE(inputMethods.count(), 1);
    QCOMPARE(dispatchOrder, QStringList({
        QStringLiteral("shutdown"),
        QStringLiteral("paste"),
        QStringLiteral("key"),
        QStringLiteral("ime"),
    }));

    // Once graceful shutdown begins, no new worker-backed configured action
    // may enter the correlated request lifecycle.
    QVERIFY(!pane.executeConfiguredAction(
        QStringLiteral("write_screen_file:open")));
    QCOMPARE(files.count(), 1);

    QCoreApplication::processEvents();
    QCOMPARE(openCount, 0);
    QCOMPARE(pasted.count(), 1);
    QCOMPARE(pasted.constFirst().constFirst().toString(), sentinel);
    QCOMPARE(files.count(), 1);
    QCOMPARE(forwarded.count(), 1);
    QCOMPARE(inputMethods.count(), 1);
}

void TerminalPaneTest::dropsPreExitSearchSelectionEffectOnSessionExit_data()
{
    QTest::addColumn<bool>("resultQueuedBeforeExit");

    QTest::newRow("result-posted-before-exit") << true;
    QTest::newRow("result-arrives-after-exit") << false;
}

void TerminalPaneTest::dropsPreExitSearchSelectionEffectOnSessionExit()
{
    QFETCH(bool, resultQueuedBeforeExit);

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("alt+b=search_selection"),
        QStringLiteral("chain=paste_from_clipboard"),
    });

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    QSignalSpy searchSelections(
        controller, &TerminalController::searchSelectionActionRequested);
    QSignalSpy searchRequests(
        controller, &TerminalController::searchRequested);
    QSignalSpy focusRequests(
        &pane, &TerminalPane::searchUiFocusRequested);
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const QString sentinel =
        QStringLiteral("pre-exit-search-selection-chain");
    clipboard->setText(sentinel, QClipboard::Clipboard);
    const auto clipboardCleanup = qScopeGuard([clipboard] {
        clipboard->clear(QClipboard::Clipboard);
    });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_B, Qt::AltModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &press);
    QKeyEvent release(
        QEvent::KeyRelease, Qt::Key_B, Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &release);
    QCOMPARE(searchSelections.count(), 1);
    QCOMPARE(pasted.count(), 0);
    QCOMPARE(forwarded.count(), 0);
    QVERIFY(!pane.searchUiActive());

    const quint64 requestId =
        searchSelections.constFirst().constFirst().toULongLong();
    QVERIFY(requestId != 0);
    const TerminalActionResult result{
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::StartSearch,
        .performed = true,
        .payload = QStringLiteral("selection from exited session"),
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    };

    if (resultQueuedBeforeExit) {
        // terminalActionReady is queued once more at the pane boundary, so
        // this result is posted but has not committed its UI effect yet.
        Q_EMIT controller->terminalActionReady(result);
        QVERIFY(!pane.searchUiActive());
    }

    // A held terminal remains alive after the child exits, allowing us to
    // observe whether stale selection-derived UI work is resurrected.
    Q_EMIT controller->sessionExited(0, 0, true, false, 500, false);
    QVERIFY(!pane.searchUiActive());

    if (!resultQueuedBeforeExit) {
        Q_EMIT controller->terminalActionReady(result);
    }

    QCoreApplication::processEvents();
    QVERIFY(!pane.searchUiActive());
    QCOMPARE(focusRequests.count(), 0);
    QCOMPARE(searchRequests.count(), 0);
    QCOMPARE(pasted.count(), 1);
    QCOMPARE(pasted.constFirst().constFirst().toString(), sentinel);
    QCOMPARE(forwarded.count(), 0);

    // The failed stale action still resolves the chain and releases its input
    // barrier; unrelated input must not remain trapped behind it.
    QKeyEvent ordinaryPress(
        QEvent::KeyPress, Qt::Key_C, Qt::NoModifier,
        QStringLiteral("c"));
    QCoreApplication::sendEvent(&pane, &ordinaryPress);
    QCOMPARE(forwarded.count(), 1);
}

void TerminalPaneTest::rejectsDuplicateTerminalActionRequestIds()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy copies(
        controller, &TerminalController::copyActionRequested);
    QSignalSpy adjustments(
        controller,
        &TerminalController::selectionAdjustmentActionRequested);
    QSignalSpy selectionScrolls(
        controller, &TerminalController::scrollToSelectionActionRequested);
    QSignalSpy searchSelections(
        controller, &TerminalController::searchSelectionActionRequested);
    QSignalSpy completions(
        controller, &TerminalController::terminalActionReady);

    constexpr quint64 copyRequestId = 42;
    constexpr quint64 adjustmentRequestId = 43;
    constexpr quint64 scrollRequestId = 44;
    constexpr quint64 searchRequestId = 45;
    QVERIFY(controller->copySelectionAction(copyRequestId));
    QVERIFY(!controller->copySelectionAction(copyRequestId));
    QVERIFY(!controller->adjustSelectionAction(
        copyRequestId, TerminalSelectionAdjustment::Left));
    QVERIFY(!controller->scrollToSelectionAction(copyRequestId));
    QVERIFY(!controller->searchSelectionAction(copyRequestId));

    QVERIFY(!controller->copySelectionAction(0));
    QVERIFY(!controller->adjustSelectionAction(
        0, TerminalSelectionAdjustment::Left));
    QVERIFY(!controller->scrollToSelectionAction(0));
    QVERIFY(!controller->searchSelectionAction(0));

    QVERIFY(controller->adjustSelectionAction(
        adjustmentRequestId, TerminalSelectionAdjustment::Right));
    QVERIFY(controller->scrollToSelectionAction(scrollRequestId));
    QVERIFY(controller->searchSelectionAction(searchRequestId));
    QCOMPARE(copies.count(), 1);
    QCOMPARE(adjustments.count(), 1);
    QCOMPARE(selectionScrolls.count(), 1);
    QCOMPARE(searchSelections.count(), 1);
    QCOMPARE(completions.count(), 0);

    controller->beginShutdown();
    QCOMPARE(completions.count(), 4);
    const QList<quint64> expectedRequestIds{
        copyRequestId,
        adjustmentRequestId,
        scrollRequestId,
        searchRequestId,
    };
    for (qsizetype index = 0; index < expectedRequestIds.size(); ++index) {
        const TerminalActionResult completion =
            qvariant_cast<TerminalActionResult>(
                completions.at(index).constFirst());
        QCOMPARE(completion.requestId, expectedRequestIds.at(index));
        QCOMPARE(completion.outcome, TerminalActionOutcome::Failed);
        QVERIFY(!completion.performed);
    }
}

void TerminalPaneTest::handlesCompletionReentrantlyDuringTerminalActionStart()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("alt+b=write_screen_file:open"),
        QStringLiteral("chain=paste_from_clipboard"),
    });

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    QClipboard *const clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    const QString clipboardText =
        QStringLiteral("reentrant-completion-clipboard");
    clipboard->setText(clipboardText, QClipboard::Clipboard);
    const auto clipboardCleanup = qScopeGuard([clipboard] {
        clipboard->clear(QClipboard::Clipboard);
    });

    const QString artifactPath =
        QStringLiteral("/tmp/reentrant-terminal-action.txt");
    QList<QUrl> openedUrls;
    pane.setUrlOpener([&openedUrls](const QUrl &url) {
        openedUrls.append(url);
        return true;
    });
    QSignalSpy pasted(controller, &TerminalController::pasteRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    connect(
        controller, &TerminalController::writeTerminalFileRequested,
        &pane,
        [controller, artifactPath](
            quint64 requestId, const TerminalWriteFileAction &) {
            Q_EMIT controller->terminalActionReady(
                successfulOpenFileResult(requestId, artifactPath));
            // Exercise a modal/nested event loop while the request signal is
            // still unwinding through startConfiguredAction().
            QCoreApplication::processEvents();
        });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_B, Qt::AltModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &press);
    QCOMPARE(
        openedUrls,
        QList<QUrl>{QUrl::fromLocalFile(artifactPath)});
    QCOMPARE(pasted.count(), 1);
    QCOMPARE(pasted.constFirst().constFirst().toString(), clipboardText);

    QKeyEvent release(
        QEvent::KeyRelease, Qt::Key_B, Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &release);
    QCOMPARE(forwarded.count(), 0);

    // A stranded continuation would retain key deferral and swallow these.
    QKeyEvent ordinaryPress(
        QEvent::KeyPress, Qt::Key_C, Qt::NoModifier,
        QStringLiteral("c"));
    QCoreApplication::sendEvent(&pane, &ordinaryPress);
    QKeyEvent ordinaryRelease(
        QEvent::KeyRelease, Qt::Key_C, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &ordinaryRelease);
    QCOMPARE(forwarded.count(), 2);
}

void TerminalPaneTest::retainsWorkerPerformedStateWhenGuiEffectFails()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral(
            "performable:alt+b=write_screen_file:open"),
    });

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy files(
        controller, &TerminalController::writeTerminalFileRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    int openAttempts = 0;
    pane.setUrlOpener([&openAttempts](const QUrl &) {
        ++openAttempts;
        return false;
    });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_B, Qt::AltModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &press);
    QCOMPARE(files.count(), 1);
    QKeyEvent release(
        QEvent::KeyRelease, Qt::Key_B, Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &release);

    const quint64 requestId =
        files.constFirst().constFirst().toULongLong();
    Q_EMIT controller->terminalActionReady(
        successfulOpenFileResult(
            requestId,
            QStringLiteral("/tmp/unhandled-terminal-action.txt")));
    QCoreApplication::processEvents();

    QCOMPARE(openAttempts, 1);
    // Artifact creation is the authoritative performed operation. A desktop
    // opener rejection must not turn a performable binding into PTY fallback.
    QCOMPARE(forwarded.count(), 0);
}

void TerminalPaneTest::defersInputMethodDuringTerminalActionChains()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("alt+b=write_screen_file:open"),
        QStringLiteral("chain=text:after"),
    });

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy files(
        controller, &TerminalController::writeTerminalFileRequested);
    QSignalSpy rawText(controller, &TerminalController::rawTextRequested);
    QSignalSpy inputMethod(
        controller, &TerminalController::inputMethodRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    QStringList order;
    pane.setUrlOpener([&order](const QUrl &) {
        order.append(QStringLiteral("open"));
        return true;
    });
    connect(
        controller, &TerminalController::rawTextRequested, &pane,
        [&order](const QByteArray &) {
            order.append(QStringLiteral("chain"));
        });
    connect(
        controller, &TerminalController::inputMethodRequested, &pane,
        [&order](const TerminalInputMethodInput &) {
            order.append(QStringLiteral("ime"));
        });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_B, Qt::AltModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &press);
    QCOMPARE(files.count(), 1);

    QInputMethodEvent inputEvent;
    inputEvent.setCommitString(QStringLiteral("committed later"));
    QCoreApplication::sendEvent(&pane, &inputEvent);
    QCOMPARE(inputMethod.count(), 0);
    QCOMPARE(rawText.count(), 0);

    QKeyEvent release(
        QEvent::KeyRelease, Qt::Key_B, Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &release);
    const quint64 requestId =
        files.constFirst().constFirst().toULongLong();
    Q_EMIT controller->terminalActionReady(
        successfulOpenFileResult(
            requestId,
            QStringLiteral("/tmp/ime-order-terminal-action.txt")));
    QCoreApplication::processEvents();

    QCOMPARE(rawText.count(), 1);
    QCOMPARE(inputMethod.count(), 1);
    const TerminalInputMethodInput committed =
        qvariant_cast<TerminalInputMethodInput>(
            inputMethod.constFirst().constFirst());
    QCOMPARE(committed.commitText, QStringLiteral("committed later"));
    QCOMPARE(
        order,
        QStringList({
            QStringLiteral("open"),
            QStringLiteral("chain"),
            QStringLiteral("ime"),
        }));
    QCOMPARE(forwarded.count(), 0);
}

void TerminalPaneTest::preservesDeferredInputFifoDuringReplayReentrancy()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("alt+b=write_screen_file:open"),
    });

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy files(
        controller, &TerminalController::writeTerminalFileRequested);
    pane.setUrlOpener([](const QUrl &) { return true; });

    QStringList order;
    bool injected = false;
    std::optional<TerminalKeyInput> replayedC;
    connect(
        controller, &TerminalController::keyRequested, &pane,
        [&](const TerminalKeyInput &input) {
            order.append(QStringLiteral("%1-%2")
                             .arg(QChar(input.key))
                             .arg(input.pressed ? QStringLiteral("press")
                                                : QStringLiteral("release")));
            if (input.key == Qt::Key_C && input.pressed) replayedC = input;
            if (injected || input.key != Qt::Key_C
                || !input.pressed) {
                return;
            }
            injected = true;
            QKeyEvent nestedKey(
                QEvent::KeyPress, Qt::Key_E, Qt::NoModifier,
                QStringLiteral("e"));
            QCoreApplication::sendEvent(&pane, &nestedKey);
            QInputMethodEvent nestedInput;
            nestedInput.setCommitString(QStringLiteral("new-ime"));
            QCoreApplication::sendEvent(&pane, &nestedInput);
        });
    connect(
        controller, &TerminalController::inputMethodRequested,
        &pane, [&order](const TerminalInputMethodInput &input) {
            order.append(
                QStringLiteral("ime:%1").arg(input.commitText));
        });

    QKeyEvent trigger(
        QEvent::KeyPress, Qt::Key_B, Qt::AltModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &trigger);
    QCOMPARE(files.count(), 1);
    QKeyEvent triggerRelease(
        QEvent::KeyRelease, Qt::Key_B, Qt::AltModifier);
    QCoreApplication::sendEvent(&pane, &triggerRelease);

    QKeyEvent cPress(
        QEvent::KeyPress, Qt::Key_C, Qt::NoModifier,
        QStringLiteral("c"));
    {
        const ScopedKeyboardLayoutTranslation scope(
            cPress,
            {
                .unshiftedCodepoint = '&',
                .consumedModifiers = Qt::GroupSwitchModifier,
                .capsLock = true,
                .numLock = true,
                .consumedCapsLock = true,
                .authoritative = true,
            });
        QCoreApplication::sendEvent(&pane, &cPress);
    }
    QInputMethodEvent oldInput;
    oldInput.setCommitString(QStringLiteral("old-ime"));
    QCoreApplication::sendEvent(&pane, &oldInput);
    QKeyEvent cRelease(
        QEvent::KeyRelease, Qt::Key_C, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &cRelease);
    QKeyEvent dPress(
        QEvent::KeyPress, Qt::Key_D, Qt::NoModifier,
        QStringLiteral("d"));
    QCoreApplication::sendEvent(&pane, &dPress);
    QVERIFY(order.isEmpty());

    const quint64 requestId =
        files.constFirst().constFirst().toULongLong();
    Q_EMIT controller->terminalActionReady(
        successfulOpenFileResult(
            requestId,
            QStringLiteral("/tmp/reentrant-input-fifo.txt")));
    QCoreApplication::processEvents();

    QVERIFY(injected);
    QVERIFY(replayedC.has_value());
    QCOMPARE(replayedC->unshiftedCodepoint, std::uint32_t{'&'});
    QCOMPARE(replayedC->consumedModifiers,
             static_cast<int>(Qt::GroupSwitchModifier));
    QVERIFY(replayedC->capsLock);
    QVERIFY(replayedC->numLock);
    QVERIFY(replayedC->consumedCapsLock);
    QCOMPARE(
        order,
        QStringList({
            QStringLiteral("C-press"),
            QStringLiteral("ime:old-ime"),
            QStringLiteral("C-release"),
            QStringLiteral("D-press"),
            QStringLiteral("E-press"),
            QStringLiteral("ime:new-ime"),
        }));
}

void TerminalPaneTest::survivesDestructionDuringDelayedActionFinalization()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral(
            "performable:alt+b=write_screen_file:copy"),
    });

    auto *pane = new TerminalPane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    const QPointer<TerminalPane> guard(pane);
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy files(
        controller, &TerminalController::writeTerminalFileRequested);
    connect(
        controller, &TerminalController::keyRequested, this,
        [pane](const TerminalKeyInput &) {
            delete pane;
        });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_B, Qt::AltModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(pane, &press);
    QCOMPARE(files.count(), 1);
    const quint64 requestId =
        files.constFirst().constFirst().toULongLong();
    Q_EMIT controller->terminalActionReady({
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Failed,
        .effect = TerminalActionEffect::None,
        .performed = false,
        .payload = {},
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    });
    QCoreApplication::processEvents();
    QVERIFY(guard.isNull());
}

void TerminalPaneTest::dropsPendingConsumedKeyOwnershipOnFocusLoss()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("alt+b=write_screen_file:open"),
    });

    TerminalPane pane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy files(
        controller, &TerminalController::writeTerminalFileRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    pane.setUrlOpener([](const QUrl &) { return true; });

    QKeyEvent press(
        QEvent::KeyPress, Qt::Key_B, Qt::AltModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &press);
    QCOMPARE(files.count(), 1);

    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(&pane, &focusOut);

    // A new press/release pair for the same logical key may arrive after the
    // pane regains focus while the old action is still pending. Its release
    // belongs to the new focus epoch and must not stand in for the original
    // binding release.
    QFocusEvent focusIn(QEvent::FocusIn, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(&pane, &focusIn);
    QKeyEvent ordinaryPress(
        QEvent::KeyPress, Qt::Key_B, Qt::NoModifier,
        QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &ordinaryPress);
    QKeyEvent ordinaryRelease(
        QEvent::KeyRelease, Qt::Key_B, Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &ordinaryRelease);
    QCOMPARE(forwarded.count(), 0);

    const quint64 requestId =
        files.constFirst().constFirst().toULongLong();
    Q_EMIT controller->terminalActionReady(
        successfulOpenFileResult(
            requestId,
            QStringLiteral("/tmp/focus-loss-terminal-action.txt")));
    QCoreApplication::processEvents();

    // The original Alt+B release went elsewhere on focus loss. Draining the
    // new epoch must forward both ordinary events without manufacturing a
    // stale consumed-key identity for either release.
    QCOMPARE(forwarded.count(), 2);
}

void TerminalPaneTest::routesTerminalFileActions()
{
    const QString printfExecutable =
        QStandardPaths::findExecutable(QStringLiteral("printf"));
    QVERIFY(!printfExecutable.isEmpty());

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {
        printfExecutable,
        QStringLiteral("pane-terminal-file-content"),
    };
    options.hold = true;

    TerminalPane pane(options);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy updates(controller, &TerminalController::terminalUpdated);
    QSignalSpy requests(
        controller, &TerminalController::writeTerminalFileRequested);
    QSignalSpy errors(controller, &TerminalController::errorOccurred);

    QList<QUrl> openedUrls;
    QString artifactDirectoryPath;
    const auto cleanupArtifact = qScopeGuard([&artifactDirectoryPath] {
        if (!artifactDirectoryPath.isEmpty()) {
            static_cast<void>(
                QDir(artifactDirectoryPath).removeRecursively());
        }
    });
    pane.setUrlOpener([&openedUrls](const QUrl &url) {
        openedUrls.append(url);
        return true;
    });

    QTRY_VERIFY_WITH_TIMEOUT(
        updatesContain(
            updates, QStringLiteral("pane-terminal-file-content")),
        5000);
    // The short-lived producer queues its exit after the final update. Wait
    // for the held pane before issuing actions so that exit processing cannot
    // invalidate otherwise valid in-flight file requests.
    QTRY_VERIFY_WITH_TIMEOUT(!controller->running(), 5000);

    // Missing selection is still a performed action. Queue a screen write
    // behind it as a worker-order barrier: receiving only screen.txt proves
    // the earlier unavailable selection did not invoke the URL opener.
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("write_selection_file:open,plain")));
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("write_screen_file:open")));
    QCOMPARE(requests.count(), 2);

    const quint64 selectionRequestId =
        requests.at(0).at(0).toULongLong();
    QVERIFY(selectionRequestId != 0);
    const TerminalWriteFileAction selectionRequest =
        qvariant_cast<TerminalWriteFileAction>(
            requests.at(0).at(1));
    QCOMPARE(selectionRequest.location, TerminalFileLocation::Selection);
    QCOMPARE(selectionRequest.disposition, TerminalFileDisposition::Open);
    QCOMPARE(selectionRequest.format, TerminalFileFormat::Plain);
    const quint64 screenRequestId =
        requests.at(1).at(0).toULongLong();
    QVERIFY(screenRequestId != 0);
    QVERIFY(screenRequestId != selectionRequestId);
    const TerminalWriteFileAction screenRequest =
        qvariant_cast<TerminalWriteFileAction>(
            requests.at(1).at(1));
    QCOMPARE(screenRequest.location, TerminalFileLocation::Screen);
    QCOMPARE(screenRequest.disposition, TerminalFileDisposition::Open);
    QCOMPARE(screenRequest.format, TerminalFileFormat::Plain);

    QTRY_COMPARE_WITH_TIMEOUT(openedUrls.size(), 1, 10000);
    const QUrl openedUrl = openedUrls.constFirst();
    QVERIFY(openedUrl.isLocalFile());
    const QFileInfo artifact(openedUrl.toLocalFile());
    artifactDirectoryPath = artifact.absolutePath();
    QVERIFY(artifact.isAbsolute());
    QCOMPARE(artifact.fileName(), QStringLiteral("screen.txt"));
    QVERIFY(artifact.isFile());

    QFile file(artifact.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(),
             QByteArrayLiteral("pane-terminal-file-content"));
    file.close();
    QVERIFY(errors.isEmpty());

    const QDir artifactDirectory = artifact.absoluteDir();
    QVERIFY(artifactDirectory.dirName().startsWith(
        QStringLiteral("ghostty-qt-")));
    QVERIFY(QDir(artifactDirectoryPath).removeRecursively());
}

void TerminalPaneTest::dropsQueuedTerminalFileOpenAfterTeardown()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;

    auto *pane = new TerminalPane(
        options, nullptr, std::nullopt,
        TerminalSessionStartMode::Deferred);
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    int openCount = 0;
    pane->setUrlOpener([&openCount](const QUrl &) {
        ++openCount;
        return true;
    });
    QSignalSpy requests(
        controller, &TerminalController::writeTerminalFileRequested);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("write_screen_file:open")));
    QCOMPARE(requests.count(), 1);
    const quint64 requestId =
        requests.constFirst().constFirst().toULongLong();
    QVERIFY(requestId != 0);

    const QPointer<TerminalPane> guardedPane(pane);
    Q_EMIT controller->terminalActionReady({
        .requestId = requestId,
        .outcome = TerminalActionOutcome::Success,
        .effect = TerminalActionEffect::OpenFile,
        .performed = true,
        .payload =
            QStringLiteral("/tmp/stale-terminal-file.txt"),
        .clipboardDestination =
            TerminalClipboardDestination::Standard,
    });

    delete pane;
    QVERIFY(guardedPane.isNull());
    QCoreApplication::processEvents();
    QCOMPARE(openCount, 0);
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
    useSystemFixedFont(options);
    options.appearance.foregroundColor = Qt::white;
    options.appearance.backgroundColor = Qt::black;
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+y=copy_url_to_clipboard"),
    });

    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography);
    const qreal cellWidth = metrics.cellWidth;
    const qreal cellHeight = metrics.cellHeight;

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
    useSystemFixedFont(options);
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+y=copy_url_to_clipboard"),
    });

    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography);
    const qreal cellWidth = metrics.cellWidth;
    const qreal cellHeight = metrics.cellHeight;

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
    QTRY_COMPARE_WITH_TIMEOUT(pane->cursor().shape(), Qt::IBeamCursor, 1000);
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
    useSystemFixedFont(options);

    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography);
    const qreal cellWidth = metrics.cellWidth;
    const qreal cellHeight = metrics.cellHeight;

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
    useSystemFixedFont(options);
    options.keybindSource = GhosttyKeybindSource::text({
        QStringLiteral("ctrl+y=copy_url_to_clipboard"),
    });

    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography);
    const qreal cellWidth = metrics.cellWidth;
    const qreal cellHeight = metrics.cellHeight;

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
    useSystemFixedFont(options);

    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography);
    const qreal cellWidth = metrics.cellWidth;
    const qreal cellHeight = metrics.cellHeight;

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
    QCOMPARE(pane.cursor().shape(), Qt::IBeamCursor);
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
    useSystemFixedFont(options);

    const TerminalCellMetrics metrics =
        terminalCellMetrics(options.typography);
    const qreal cellWidth = metrics.cellWidth;
    const qreal cellHeight = metrics.cellHeight;
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

    // The raw DEC state also governs link normalization while the independent
    // frontend reporting gate is off. Always removes the stationary local
    // lease; Never restores it without reporting a DEC motion event.
    LaunchOptions disabledCapture = options;
    disabledCapture.mouseShiftCapture = MouseShiftCapture::Always;
    pane.applyRuntimeOptions(disabledCapture);
    QVERIFY(controller->terminalMouseTracking());
    QVERIFY(!controller->mouseTracking());
    QTRY_COMPARE_WITH_TIMEOUT(pane.cursor().shape(), Qt::IBeamCursor, 1000);
    const qsizetype disabledQueriesBeforeNever = hyperlinkQueries.size();
    disabledCapture.mouseShiftCapture = MouseShiftCapture::Never;
    pane.applyRuntimeOptions(disabledCapture);
    QTRY_VERIFY_WITH_TIMEOUT(
        hyperlinkQueries.size() > disabledQueriesBeforeNever, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(pane.cursor().shape(), Qt::PointingHandCursor,
                              1000);
    QCOMPARE(mouseRequests.count(), 0);

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

    // Reload resolves a stationary Ctrl+Shift hover immediately. Capture
    // removes the local link lease without waiting for pointer motion, while
    // release recomputes the same stored cell and modifiers.
    QKeyEvent reloadControlPress(QEvent::KeyPress, Qt::Key_Control,
                                 Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &reloadControlPress);
    QKeyEvent reloadShiftPress(QEvent::KeyPress, Qt::Key_Shift,
                               Qt::ControlModifier | Qt::ShiftModifier);
    QCoreApplication::sendEvent(&pane, &reloadShiftPress);
    sendHover(Qt::NoModifier);
    QTRY_COMPARE_WITH_TIMEOUT(pane.cursor().shape(), Qt::PointingHandCursor,
                              1000);

    LaunchOptions captureReload = options;
    captureReload.mouseReporting = true;
    captureReload.mouseShiftCapture = MouseShiftCapture::Always;
    pane.applyRuntimeOptions(captureReload);
    QTRY_COMPARE_WITH_TIMEOUT(pane.cursor().shape(), Qt::IBeamCursor, 1000);
    QVERIFY(
        !pane.executeConfiguredAction(QStringLiteral("copy_url_to_clipboard")));

    const qsizetype queriesBeforeReleaseReload = hyperlinkQueries.size();
    captureReload.mouseShiftCapture = MouseShiftCapture::Never;
    pane.applyRuntimeOptions(captureReload);
    QTRY_VERIFY_WITH_TIMEOUT(
        hyperlinkQueries.size() > queriesBeforeReleaseReload, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(pane.cursor().shape(), Qt::PointingHandCursor,
                              1000);

    QKeyEvent reloadShiftRelease(QEvent::KeyRelease, Qt::Key_Shift,
                                 Qt::ControlModifier);
    QCoreApplication::sendEvent(&pane, &reloadShiftRelease);
    QKeyEvent reloadControlRelease(QEvent::KeyRelease, Qt::Key_Control,
                                   Qt::NoModifier);
    QCoreApplication::sendEvent(&pane, &reloadControlRelease);

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
        QStringLiteral("printf '%b' '\\033]0;metadata-title\\007"
                       "\\033]7;file://localhost/raw-%80-%ff\\007"
                       "metadata-ready\\n'; sleep 5"),
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
    const QByteArray rawDirectory = QByteArray::fromHex("2f7261772d802dff");
    QTRY_COMPARE_WITH_TIMEOUT(pane.currentDirectoryBytes(), rawDirectory, 1000);
    QCOMPARE(pane.splitLaunchOptions(options).workingDirectory.bytes(),
             rawDirectory);
    QCOMPARE(pane.tabLaunchOptions(options).workingDirectory.bytes(),
             rawDirectory);
    QCOMPARE(pane.windowLaunchOptions(options).workingDirectory.bytes(),
             rawDirectory);

    QVERIFY(pane.executeConfiguredAction(QStringLiteral("reset")));
    QTRY_VERIFY_WITH_TIMEOUT(pane.currentDirectory().isEmpty(), 1000);
    QCOMPARE(pane.title(), QStringLiteral("metadata-title"));
    QCOMPARE(pane.splitLaunchOptions(options).workingDirectory,
             QDir::tempPath());

    // OSC and set_surface_title converge on the same base-title layer.
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
    GhosttyKeybindConfig config;
    config.root = {
        sequence('n', QStringLiteral("new_tab")),
        sequence('u', QStringLiteral("reload_config"),
                 GhosttyKeybindFlags{.consumed = false}),
        sequence('p', QStringLiteral("goto_split:left"),
                 GhosttyKeybindFlags{.performable = true}),
        sequence('e', QStringLiteral("end_key_sequence")),
        sequence('f', QStringLiteral("end_key_sequence"),
                 GhosttyKeybindFlags{.consumed = false}),
    };
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));

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
    QSignalSpy keySequenceChanges(&pane,
                                  &TerminalPane::pendingKeySequenceChanged);

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
    QCOMPARE(pane.pendingKeySequence(),
             QStringList({QStringLiteral("Ctrl+X")}));
    QCOMPARE(keySequenceChanges.count(), 1);
    release(Qt::Key_X, Qt::ControlModifier, QString(QChar(0x18)));
    QCOMPARE(forwarded.count(), 1);
    press(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(newTab.count(), 1);
    QCOMPARE(resolution(), TerminalSequenceResolution::Drop);
    QVERIFY(pane.pendingKeySequence().isEmpty());
    QCOMPARE(keySequenceChanges.count(), 2);
    release(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(forwarded.count(), 1);

    // An invalid continuation atomically flushes the encoded prefix and the
    // current press; it must not also travel through the ordinary key signal.
    leader();
    press(Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"));
    QCOMPARE(resolution(),
             TerminalSequenceResolution::FlushAndSendCurrent);
    QVERIFY(pane.pendingKeySequence().isEmpty());
    QCOMPARE(keySequenceChanges.count(), 4);
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
    QVERIFY(
        pane.controlInspector(WorkspaceFrontendActions::InspectorMode::Show));
    leader();
    press(Qt::Key_E, Qt::NoModifier, QStringLiteral("e"));
    QCOMPARE(resolution(), TerminalSequenceResolution::Flush);
    QVERIFY(!resolved.constLast().at(2).toBool());
    const TerminalKeyInput endSequenceTrace =
        qvariant_cast<TerminalKeyInput>(resolved.constLast().at(3));
    QVERIFY(endSequenceTrace.inspectorTraceGeneration != 0);
    QVERIFY(endSequenceTrace.inspectorTraceId != 0);
    QCOMPARE(endSequenceTrace.text, QStringLiteral("e"));
    QVERIFY(
        pane.controlInspector(WorkspaceFrontendActions::InspectorMode::Hide));

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
    const int keySequenceChangesBeforeReload = keySequenceChanges.count();
    QCOMPARE(pane.pendingKeySequence(),
             QStringList({QStringLiteral("Ctrl+X")}));
    LaunchOptions reloaded = options;
    GhosttyKeybindConfig reloadedConfig;
    reloadedConfig.root = {
        GhosttyKeybindDefinition{
            .sequence = {unicode('q', GhosttyKeybindCtrl)},
            .actions = {QStringLiteral("new_tab")},
        },
    };
    reloaded.keybindSource =
        GhosttyKeybindSource::structured(std::move(reloadedConfig));
    pane.applyRuntimeOptions(reloaded);
    QVERIFY(pane.pendingKeySequence().isEmpty());
    QCOMPARE(keySequenceChanges.count(), keySequenceChangesBeforeReload + 1);
    QCOMPARE(resolved.count(), resolutionsBeforeReload + 1);
    QCOMPARE(resolution(), TerminalSequenceResolution::Drop);
    const int beforeFormerLeaf = forwarded.count();
    press(Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
    QCOMPARE(forwarded.count(), beforeFormerLeaf + 1);
}

void TerminalPaneTest::preservesStateWithinAKeybindProgramGeneration()
{
    const GhosttyKeybindConfig config = generationTestConfig();
    const GhosttyKeybindProgram generation =
        GhosttyKeybindProgram::compile(config).program;

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);

    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, generation);
    QVERIFY(pane.keybindProgram().isSameGeneration(generation));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy staged(controller,
                      &TerminalController::sequenceKeyStagingRequested);
    QSignalSpy resolved(controller,
                        &TerminalController::sequenceResolutionRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);
    QSignalSpy tableChanges(&pane, &TerminalPane::activeKeyTablesChanged);

    const auto press = [&pane](int key, Qt::KeyboardModifiers modifiers,
                               QString text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };
    const auto lastResolution = [&resolved] {
        return qvariant_cast<TerminalSequenceResolution>(
            resolved.constLast().at(1));
    };

    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("activate_key_table:edit")));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edit")}));
    QCOMPARE(tableChanges.count(), 1);
    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCOMPARE(staged.count(), 1);
    QCOMPARE(resolved.count(), 0);

    // Runtime policy may change without changing the compiled generation.
    // The pane-local table stack and staged traversal must survive it.
    LaunchOptions updated = options;
    updated.linkPreviews = LinkPreviewMode::Never;
    pane.applyRuntimeOptions(updated, generation);
    QVERIFY(pane.keybindProgram().isSameGeneration(generation));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edit")}));
    QCOMPARE(tableChanges.count(), 1);
    QCOMPARE(resolved.count(), 0);

    press(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    QCOMPARE(newTab.count(), 1);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(lastResolution(), TerminalSequenceResolution::Drop);
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edit")}));

    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCOMPARE(staged.count(), 2);
    const quint64 stagedToken = staged.constLast().at(0).toULongLong();
    const int resolutionsBeforeReplacement = resolved.count();

    // Recompiling equal input deliberately creates a new generation. Its
    // identity change clears surface-local state and drops worker staging
    // exactly once; structural equality must not preserve stale traversal.
    const GhosttyKeybindProgram equalReplacement =
        GhosttyKeybindProgram::compile(config).program;
    QVERIFY(!generation.isSameGeneration(equalReplacement));
    QCOMPARE(generation.serializedActions(),
             equalReplacement.serializedActions());
    pane.applyRuntimeOptions(updated, equalReplacement);
    QVERIFY(pane.keybindProgram().isSameGeneration(equalReplacement));
    QVERIFY(pane.activeKeyTables().isEmpty());
    QCOMPARE(tableChanges.count(), 2);
    QCOMPARE(resolved.count(), resolutionsBeforeReplacement + 1);
    QCOMPARE(resolved.constLast().at(0).toULongLong(), stagedToken);
    QCOMPARE(lastResolution(), TerminalSequenceResolution::Drop);

    pane.applyRuntimeOptions(updated, equalReplacement);
    QCOMPARE(tableChanges.count(), 2);
    QCOMPARE(resolved.count(), resolutionsBeforeReplacement + 1);

    const int forwardedBeforeFormerContinuation = forwarded.count();
    press(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    QCOMPARE(forwarded.count(), forwardedBeforeFormerContinuation + 1);
    QCOMPARE(newTab.count(), 1);
    QCOMPARE(resolved.count(), resolutionsBeforeReplacement + 1);
}

void TerminalPaneTest::newerSameProgramRuntimeUpdateWinsReentry()
{
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(GhosttyKeybindConfig{}).program;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured({});
    options.resizeOverlay.position = ResizeOverlayPosition::Center;

    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, program);
    pane.setSize(QSizeF(500.0, 300.0));
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy runtimeOptions(
        controller, &TerminalController::runtimeOptionsRequested);

    LaunchOptions outer = options;
    outer.resizeOverlay.position = ResizeOverlayPosition::TopLeft;
    outer.selectionClipboard.trimTrailingSpaces = false;
    LaunchOptions newer = options;
    newer.resizeOverlay.position = ResizeOverlayPosition::BottomRight;
    newer.selectionClipboard.copyOnSelect =
        TerminalCopyOnSelectMode::Disabled;

    bool nested = false;
    connect(&pane, &TerminalPane::resizeOverlayRectChanged,
            &pane, [&] {
                if (nested) return;
                nested = true;
                pane.applyRuntimeOptions(newer, program);
            });

    pane.applyRuntimeOptions(outer, program);

    QVERIFY(nested);
    QVERIFY(pane.keybindProgram().isSameGeneration(program));
    QCOMPARE(pane.resizeOverlayRect().topLeft(), QPointF(380.0, 260.0));
    QCOMPARE(runtimeOptions.count(), 2);
    QCOMPARE(qvariant_cast<TerminalSessionRuntimeOptions>(
                 runtimeOptions.constLast().constFirst()),
             toTerminalSessionRuntimeOptions(newer));
}

void TerminalPaneTest::disabledMouseHideWinsSameProgramRuntimeReentry()
{
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(GhosttyKeybindConfig{}).program;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.mouseHideWhileTyping = true;
    options.keybindSource = GhosttyKeybindSource::structured({});

    TerminalPane pane(options, nullptr, std::nullopt,
                      TerminalSessionStartMode::Immediate, {}, program);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);

    QKeyEvent hide(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
                   QStringLiteral("a"));
    QCoreApplication::sendEvent(&pane, &hide);
    QCOMPARE(pane.cursor().shape(), Qt::BlankCursor);

    LaunchOptions disabled = options;
    disabled.mouseHideWhileTyping = false;
    disabled.selectionClipboard.trimTrailingSpaces = false;

    bool nested = false;
    connect(
        controller, &TerminalController::runtimeOptionsRequested, &pane,
        [&](const TerminalSessionRuntimeOptions &) {
            if (nested) return;
            nested = true;
            pane.applyRuntimeOptions(disabled, program);
        },
        Qt::DirectConnection);

    pane.applyRuntimeOptions(disabled, program);

    QVERIFY(nested);
    QCOMPARE(pane.cursor().shape(), Qt::IBeamCursor);
    QKeyEvent remainsVisible(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier,
                             QStringLiteral("b"));
    QCoreApplication::sendEvent(&pane, &remainsVisible);
    QCOMPARE(pane.cursor().shape(), Qt::IBeamCursor);
}

void TerminalPaneTest::keyTableResetNotifiesBeforeLaterReentry()
{
    const GhosttyKeybindConfig config = generationTestConfig();
    const GhosttyKeybindProgram initialProgram =
        GhosttyKeybindProgram::compile(config).program;
    const GhosttyKeybindProgram outerProgram =
        GhosttyKeybindProgram::compile(config).program;
    const GhosttyKeybindProgram newerProgram =
        GhosttyKeybindProgram::compile(config).program;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);

    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, initialProgram);
    QSignalSpy tableChanges(&pane, &TerminalPane::activeKeyTablesChanged);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("activate_key_table:edit")));
    QCOMPARE(tableChanges.count(), 1);

    LaunchOptions outer = options;
    outer.resizeOverlay.position = ResizeOverlayPosition::TopLeft;
    LaunchOptions newer = options;
    newer.resizeOverlay.position = ResizeOverlayPosition::BottomRight;
    bool nested = false;
    connect(&pane, &TerminalPane::resizeOverlayRectChanged,
            &pane, [&] {
                if (nested) return;
                nested = true;
                pane.applyRuntimeOptions(newer, newerProgram);
            });

    pane.applyRuntimeOptions(outer, outerProgram);

    QVERIFY(nested);
    QCOMPARE(tableChanges.count(), 2);
    QVERIFY(pane.activeKeyTables().isEmpty());
    QVERIFY(pane.keybindProgram().isSameGeneration(newerProgram));
}

void TerminalPaneTest::runtimeOptionsObserverMayDestroyPane()
{
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(options.keybindSource).program;

    auto *pane = new TerminalPane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, program);
    const QPointer<TerminalPane> guardedPane(pane);
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    connect(controller, &TerminalController::runtimeOptionsRequested,
            this, [pane] { delete pane; });

    LaunchOptions updated = options;
    updated.selectionClipboard.trimTrailingSpaces = false;
    pane->applyRuntimeOptions(updated, program);

    QVERIFY(guardedPane.isNull());
}

void TerminalPaneTest::reloadsSafelyFromSequenceStagingNotification()
{
    qRegisterMetaType<TerminalSequenceResolution>();

    const GhosttyKeybindConfig config = generationTestConfig();
    const GhosttyKeybindProgram initialGeneration =
        GhosttyKeybindProgram::compile(config).program;
    const GhosttyKeybindProgram replacementGeneration =
        GhosttyKeybindProgram::compile(config).program;

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);

    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, initialGeneration);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy staged(controller,
                      &TerminalController::sequenceKeyStagingRequested);
    QSignalSpy resolved(controller,
                        &TerminalController::sequenceResolutionRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy tableChanges(&pane, &TerminalPane::activeKeyTablesChanged);

    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("activate_key_table_once:once")));
    QCOMPARE(tableChanges.count(), 1);
    bool reloaded = false;
    connect(controller, &TerminalController::sequenceKeyStagingRequested,
            &pane, [&] {
                if (reloaded) return;
                reloaded = true;
                pane.applyRuntimeOptions(options, replacementGeneration);
            });

    QKeyEvent leader(
        QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCoreApplication::sendEvent(&pane, &leader);

    QVERIFY(reloaded);
    QVERIFY(pane.keybindProgram().isSameGeneration(replacementGeneration));
    QVERIFY(pane.activeKeyTables().isEmpty());
    QCOMPARE(tableChanges.count(), 2);
    QCOMPARE(staged.count(), 1);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(resolved.constFirst().at(0), staged.constFirst().at(0));
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 resolved.constFirst().at(1)),
             TerminalSequenceResolution::Drop);

    const int forwardedBeforeFormerContinuation = forwarded.count();
    QKeyEvent continuation(
        QEvent::KeyPress, Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    QCoreApplication::sendEvent(&pane, &continuation);
    QCOMPARE(forwarded.count(), forwardedBeforeFormerContinuation + 1);
    QCOMPARE(resolved.count(), 1);
}

void TerminalPaneTest::sequenceStagingObserverMayDestroyPane()
{
    const GhosttyKeybindConfig config = generationTestConfig();
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(config).program;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);

    auto *pane = new TerminalPane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Deferred,
        {}, program);
    const QPointer<TerminalPane> guardedPane(pane);
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("activate_key_table:edit")));
    connect(controller, &TerminalController::sequenceKeyStagingRequested,
            this, [pane] { delete pane; });

    QKeyEvent leader(
        QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCoreApplication::sendEvent(pane, &leader);

    QVERIFY(guardedPane.isNull());
}

void TerminalPaneTest::sequenceResolutionObserverMayDestroyPane()
{
    const GhosttyKeybindConfig config = generationTestConfig();
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(config).program;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);

    auto *pane = new TerminalPane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Deferred,
        {}, program);
    const QPointer<TerminalPane> guardedPane(pane);
    auto *controller = pane->findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QVERIFY(pane->executeConfiguredAction(
        QStringLiteral("activate_key_table:edit")));

    QKeyEvent leader(
        QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCoreApplication::sendEvent(pane, &leader);
    connect(controller, &TerminalController::sequenceResolutionRequested,
            this, [pane] { delete pane; });

    QKeyEvent invalid(
        QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"));
    QCoreApplication::sendEvent(pane, &invalid);

    QVERIFY(guardedPane.isNull());
}

void TerminalPaneTest::reloadsSafelyFromOneShotLeaderTableNotification()
{
    const GhosttyKeybindConfig config = generationTestConfig();
    const GhosttyKeybindProgram initialGeneration =
        GhosttyKeybindProgram::compile(config).program;
    const GhosttyKeybindProgram replacementGeneration =
        GhosttyKeybindProgram::compile(config).program;
    QVERIFY(!initialGeneration.isSameGeneration(replacementGeneration));

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);

    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, initialGeneration);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy staged(controller,
                      &TerminalController::sequenceKeyStagingRequested);
    QSignalSpy resolved(controller,
                        &TerminalController::sequenceResolutionRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);
    QSignalSpy tableChanges(&pane, &TerminalPane::activeKeyTablesChanged);

    const auto press = [&pane](int key, Qt::KeyboardModifiers modifiers,
                               QString text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };
    press(Qt::Key_O, Qt::ControlModifier, QString(QChar(0x0f)));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("once")}));
    QCOMPARE(tableChanges.count(), 1);

    bool reloadedFromObserver = false;
    connect(&pane, &TerminalPane::activeKeyTablesChanged, &pane, [&] {
        if (reloadedFromObserver || !pane.activeKeyTables().isEmpty()) return;
        reloadedFromObserver = true;
        pane.applyRuntimeOptions(options, replacementGeneration);
    });

    // The leader reaches worker staging before the table notification. A
    // synchronous reload then drops that exact token and cannot leave either
    // side of the sequence protocol orphaned.
    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QVERIFY(reloadedFromObserver);
    QVERIFY(pane.keybindProgram().isSameGeneration(replacementGeneration));
    QVERIFY(pane.activeKeyTables().isEmpty());
    QCOMPARE(tableChanges.count(), 2);
    QCOMPARE(staged.count(), 1);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(resolved.constFirst().at(0), staged.constFirst().at(0));
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 resolved.constFirst().at(1)),
             TerminalSequenceResolution::Drop);

    const int forwardedBeforeFormerContinuation = forwarded.count();
    press(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    QCOMPARE(forwarded.count(), forwardedBeforeFormerContinuation + 1);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(newTab.count(), 0);

    // The replacement generation remains usable immediately, with a fresh
    // token and independent traversal rather than the stale staged prefix.
    press(Qt::Key_O, Qt::ControlModifier, QString(QChar(0x0f)));
    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCOMPARE(staged.count(), 2);
    press(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    QCOMPARE(newTab.count(), 1);
    QCOMPARE(resolved.count(), 2);
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 resolved.constLast().at(1)),
             TerminalSequenceResolution::Drop);
}

void TerminalPaneTest::oneShotTableObserverCanContinueSequence()
{
    const GhosttyKeybindConfig config = generationTestConfig();
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(config).program;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);

    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, program);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy staged(controller,
                      &TerminalController::sequenceKeyStagingRequested);
    QSignalSpy resolved(controller,
                        &TerminalController::sequenceResolutionRequested);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);

    const auto press = [&pane](int key, Qt::KeyboardModifiers modifiers,
                               QString text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };
    press(Qt::Key_O, Qt::ControlModifier, QString(QChar(0x0f)));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("once")}));

    QStringList events;
    connect(controller, &TerminalController::sequenceKeyStagingRequested,
            &pane, [&events] { events.append(QStringLiteral("stage")); });
    connect(controller, &TerminalController::sequenceResolutionRequested,
            &pane, [&events] { events.append(QStringLiteral("resolve")); });
    bool continuationInjected = false;
    connect(&pane, &TerminalPane::activeKeyTablesChanged, &pane, [&] {
        if (continuationInjected || !pane.activeKeyTables().isEmpty()) return;
        continuationInjected = true;
        events.append(QStringLiteral("table"));
        press(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    });

    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));

    QVERIFY(continuationInjected);
    QCOMPARE(events, QStringList({QStringLiteral("stage"),
                                  QStringLiteral("table"),
                                  QStringLiteral("resolve")}));
    QCOMPARE(staged.count(), 1);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(resolved.constFirst().at(0), staged.constFirst().at(0));
    QCOMPARE(newTab.count(), 1);

    const int forwardedBeforeOrdinaryKey = forwarded.count();
    press(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    QCOMPARE(forwarded.count(), forwardedBeforeOrdinaryKey + 1);
    QCOMPARE(resolved.count(), 1);
}

void TerminalPaneTest::reloadResolutionObserverUsesReplacementProgram()
{
    const GhosttyKeybindConfig initialConfig = generationTestConfig();
    GhosttyKeybindConfig replacementConfig;
    replacementConfig.root = {
        GhosttyKeybindDefinition{
            .sequence = {generationTestKey('q'), generationTestKey('r')},
            .actions = {QStringLiteral("new_tab")},
        },
        GhosttyKeybindDefinition{
            .sequence = {generationTestKey('s')},
            .actions = {QStringLiteral("reset_font_size")},
        },
    };
    const GhosttyKeybindProgram initialProgram =
        GhosttyKeybindProgram::compile(initialConfig).program;
    const GhosttyKeybindProgram replacementProgram =
        GhosttyKeybindProgram::compile(replacementConfig).program;

    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(initialConfig);
    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, initialProgram);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy staged(controller,
                      &TerminalController::sequenceKeyStagingRequested);
    QSignalSpy resolved(controller,
                        &TerminalController::sequenceResolutionRequested);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);

    const auto press = [&pane](int key, Qt::KeyboardModifiers modifiers,
                               QString text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("set_font_size:20")));
    QCOMPARE(pane.fontPointSize(), 20.0);
    QVERIFY(pane.executeConfiguredAction(
        QStringLiteral("activate_key_table:edit")));
    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCOMPARE(staged.count(), 1);
    const quint64 initialToken = staged.constFirst().at(0).toULongLong();

    bool replacementLeaderInjected = false;
    connect(controller, &TerminalController::sequenceResolutionRequested,
            &pane, [&] {
                if (replacementLeaderInjected) return;
                replacementLeaderInjected = true;
                press(Qt::Key_S, Qt::NoModifier, QStringLiteral("s"));
                press(Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
            });
    LaunchOptions replacementOptions = options;
    replacementOptions.typography.pointSize = 17.0;
    replacementOptions.keybindSource =
        GhosttyKeybindSource::structured(replacementConfig);
    pane.applyRuntimeOptions(replacementOptions, replacementProgram);

    QVERIFY(replacementLeaderInjected);
    QVERIFY(pane.keybindProgram().isSameGeneration(replacementProgram));
    QVERIFY(pane.activeKeyTables().isEmpty());
    QCOMPARE(pane.fontPointSize(), 17.0);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(resolved.constFirst().at(0).toULongLong(), initialToken);
    QCOMPARE(staged.count(), 2);
    const quint64 replacementToken = staged.constLast().at(0).toULongLong();
    QVERIFY(replacementToken != initialToken);

    press(Qt::Key_R, Qt::NoModifier, QStringLiteral("r"));
    QCOMPARE(newTab.count(), 1);
    QCOMPARE(resolved.count(), 2);
    QCOMPARE(resolved.constLast().at(0).toULongLong(), replacementToken);
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 resolved.constLast().at(1)),
             TerminalSequenceResolution::Drop);
}

void TerminalPaneTest::oneShotLeafObserverCanStartSequence()
{
    GhosttyKeybindConfig config;
    config.root = {
        generationTestBinding(
            {generationTestKey('o', GhosttyKeybindCtrl)},
            QStringLiteral("activate_key_table_once:once")),
        generationTestBinding(
            {generationTestKey('q'), generationTestKey('r')},
            QStringLiteral("new_tab")),
    };
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("once"),
            .bindings = {
                generationTestBinding(
                    {generationTestKey('x')}, QStringLiteral("new_tab")),
            },
        },
    };
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(config).program;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);
    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, program);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy staged(controller,
                      &TerminalController::sequenceKeyStagingRequested);
    QSignalSpy resolved(controller,
                        &TerminalController::sequenceResolutionRequested);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);

    const auto press = [&pane](int key, Qt::KeyboardModifiers modifiers,
                               QString text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };
    press(Qt::Key_O, Qt::ControlModifier, QString(QChar(0x0f)));
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("once")}));

    bool nestedLeaderStarted = false;
    connect(&pane, &TerminalPane::activeKeyTablesChanged, &pane, [&] {
        if (nestedLeaderStarted || !pane.activeKeyTables().isEmpty()) return;
        nestedLeaderStarted = true;
        press(Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    });
    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));

    QVERIFY(nestedLeaderStarted);
    QCOMPARE(newTab.count(), 1);
    QCOMPARE(staged.count(), 1);
    QCOMPARE(resolved.count(), 0);

    press(Qt::Key_R, Qt::NoModifier, QStringLiteral("r"));
    QCOMPARE(newTab.count(), 2);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(resolved.constFirst().at(0), staged.constFirst().at(0));
}

void TerminalPaneTest::actionObserverCanStartIndependentSequence()
{
    GhosttyKeybindConfig config;
    config.root = {
        generationTestBinding(
            {generationTestKey('x'), generationTestKey('y')},
            QStringLiteral("activate_key_table:edit")),
        generationTestBinding(
            {generationTestKey('q'), generationTestKey('r')},
            QStringLiteral("new_tab")),
    };
    config.root.front().flags.consumed = false;
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("edit"),
            .bindings = {
                generationTestBinding(
                    {generationTestKey('z')}, QStringLiteral("ignore")),
            },
        },
    };
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(config).program;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);
    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, program);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy staged(controller,
                      &TerminalController::sequenceKeyStagingRequested);
    QSignalSpy resolved(controller,
                        &TerminalController::sequenceResolutionRequested);
    QSignalSpy newTab(&pane, &TerminalPane::requestNewTab);

    const auto press = [&pane](int key, Qt::KeyboardModifiers modifiers,
                               QString text) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, std::move(text));
        QCoreApplication::sendEvent(&pane, &event);
    };
    press(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCOMPARE(staged.count(), 1);
    const quint64 outerToken = staged.constFirst().at(0).toULongLong();

    bool nestedLeaderStarted = false;
    connect(&pane, &TerminalPane::activeKeyTablesChanged, &pane, [&] {
        if (nestedLeaderStarted) return;
        nestedLeaderStarted = true;
        press(Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    });
    press(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));

    QVERIFY(nestedLeaderStarted);
    QCOMPARE(pane.activeKeyTables(), QStringList({QStringLiteral("edit")}));
    QCOMPARE(staged.count(), 2);
    const quint64 nestedToken = staged.constLast().at(0).toULongLong();
    QVERIFY(nestedToken != outerToken);
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(resolved.constFirst().at(0).toULongLong(), outerToken);
    QCOMPARE(qvariant_cast<TerminalSequenceResolution>(
                 resolved.constFirst().at(1)),
             TerminalSequenceResolution::FlushAndSendCurrent);

    press(Qt::Key_R, Qt::NoModifier, QStringLiteral("r"));
    QCOMPARE(newTab.count(), 1);
    QCOMPARE(resolved.count(), 2);
    QCOMPARE(resolved.constLast().at(0).toULongLong(), nestedToken);
}

void TerminalPaneTest::deferredUnconsumedKeysPreserveDispatchOrder()
{
    GhosttyKeybindConfig config;
    config.root = {
        generationTestBinding(
            {generationTestKey('a')},
            QStringLiteral("activate_key_table:edit")),
    };
    config.root.front().flags.consumed = false;
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("edit"),
            .bindings = {
                generationTestBinding(
                    {generationTestKey('z')}, QStringLiteral("ignore")),
            },
        },
    };
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(config).program;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);
    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, program);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    connect(&pane, &TerminalPane::activeKeyTablesChanged, &pane, [&] {
        QKeyEvent press(
            QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier,
            QStringLiteral("q"));
        QCoreApplication::sendEvent(&pane, &press);
        QKeyEvent release(
            QEvent::KeyRelease, Qt::Key_Q, Qt::NoModifier,
            QStringLiteral("q"));
        QCoreApplication::sendEvent(&pane, &release);
    }, Qt::SingleShotConnection);

    QKeyEvent outer(
        QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
        QStringLiteral("a"));
    QCoreApplication::sendEvent(&pane, &outer);

    QCOMPARE(forwarded.count(), 3);
    QCOMPARE(qvariant_cast<TerminalKeyInput>(forwarded.at(0).constFirst()).text,
             QStringLiteral("a"));
    QVERIFY(qvariant_cast<TerminalKeyInput>(
        forwarded.at(0).constFirst()).pressed);
    QCOMPARE(qvariant_cast<TerminalKeyInput>(forwarded.at(1).constFirst()).text,
             QStringLiteral("q"));
    QVERIFY(qvariant_cast<TerminalKeyInput>(
        forwarded.at(1).constFirst()).pressed);
    QCOMPARE(qvariant_cast<TerminalKeyInput>(forwarded.at(2).constFirst()).text,
             QStringLiteral("q"));
    QVERIFY(!qvariant_cast<TerminalKeyInput>(
        forwarded.at(2).constFirst()).pressed);
}

void TerminalPaneTest::deferredReleaseWaitsForConsumedPress()
{
    GhosttyKeybindConfig config;
    config.root = {
        generationTestBinding(
            {generationTestKey('a')},
            QStringLiteral("activate_key_table:edit")),
    };
    config.tables = {
        GhosttyKeybindTable{
            .name = QStringLiteral("edit"),
            .bindings = {
                generationTestBinding(
                    {generationTestKey('z')}, QStringLiteral("ignore")),
            },
        },
    };
    const GhosttyKeybindProgram program =
        GhosttyKeybindProgram::compile(config).program;
    LaunchOptions options;
    options.workingDirectory = QDir::tempPath();
    options.program = {QStringLiteral("/bin/true")};
    options.hold = true;
    options.keybindSource = GhosttyKeybindSource::structured(config);
    TerminalPane pane(
        options, nullptr, std::nullopt, TerminalSessionStartMode::Immediate,
        {}, program);
    auto *controller = pane.findChild<TerminalController *>();
    QVERIFY(controller != nullptr);
    QSignalSpy forwarded(controller, &TerminalController::keyRequested);

    connect(&pane, &TerminalPane::activeKeyTablesChanged, &pane, [&] {
        QKeyEvent release(
            QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier,
            QStringLiteral("a"));
        QCoreApplication::sendEvent(&pane, &release);
    }, Qt::SingleShotConnection);

    QKeyEvent outer(
        QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
        QStringLiteral("a"));
    QCoreApplication::sendEvent(&pane, &outer);

    QVERIFY(forwarded.isEmpty());
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
    GhosttyKeybindConfig config;
    config.root = {
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
    config.tables = {
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
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));

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
    GhosttyKeybindConfig reloadedConfig =
        *reloaded.keybindSource.structured();
    reloadedConfig.tables.clear();
    reloaded.keybindSource =
        GhosttyKeybindSource::structured(std::move(reloadedConfig));
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
    GhosttyKeybindConfig config;
    config.root = {GhosttyKeybindDefinition{
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
    options.keybindSource =
        GhosttyKeybindSource::structured(std::move(config));

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
