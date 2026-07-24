#pragma once

#include "application_action.h"
#include "ghostty_keybind_set.h"
#include "key_event_snapshot.h"
#include "launch_options.h"
#include "revision_counter.h"
#include "terminal_action_result.h"
#include "terminal_cell_metrics.h"
#include "terminal_types.h"
#include "window_navigation_action.h"
#include "workspace_action.h"

#include <QByteArray>
#include <QFont>
#include <QHash>
#include <QMetaObject>
#include <QMutex>
#include <QPoint>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

class QChronoTimer;
class QFocusEvent;
class QEvent;
class QHoverEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QSGNode;
class QTimer;
class QWheelEvent;
class QQuickWindow;
class GhosttyApplicationKeybindings;
class InitialSessionCoordinator;
class TerminalController;

class TerminalPane final : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(TerminalPane)
    QML_UNCREATABLE("TerminalPane instances are owned by TerminalWorkspace")
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString currentDirectory READ currentDirectory NOTIFY currentDirectoryChanged)
    Q_PROPERTY(qreal fontPointSize READ fontPointSize NOTIFY fontPointSizeChanged)
    Q_PROPERTY(QStringList activeKeyTables READ activeKeyTables NOTIFY activeKeyTablesChanged)
    Q_PROPERTY(QString linkPreviewText READ linkPreviewText NOTIFY linkPreviewChanged)
    Q_PROPERTY(QRectF linkPreviewRect READ linkPreviewRect NOTIFY linkPreviewChanged)
    Q_PROPERTY(bool searchUiActive READ searchUiActive NOTIFY searchUiActiveChanged)
    Q_PROPERTY(QString searchUiText READ searchUiText NOTIFY searchUiTextChanged)
    Q_PROPERTY(QString searchMatchLabel READ searchMatchLabel NOTIFY searchMatchLabelChanged)
    Q_PROPERTY(bool readOnly READ isReadOnly NOTIFY readOnlyChanged)
    Q_PROPERTY(bool resizeOverlayVisible READ resizeOverlayVisible
               NOTIFY resizeOverlayVisibleChanged)
    Q_PROPERTY(QString resizeOverlayText READ resizeOverlayText
               NOTIFY resizeOverlayTextChanged)
    Q_PROPERTY(QRectF resizeOverlayRect READ resizeOverlayRect
               NOTIFY resizeOverlayRectChanged)
    Q_PROPERTY(
        bool scrollbarVisible READ scrollbarVisible NOTIFY scrollbarChanged)
    Q_PROPERTY(
        qreal scrollbarPosition READ scrollbarPosition NOTIFY scrollbarChanged)
    Q_PROPERTY(qreal scrollbarSize READ scrollbarSize NOTIFY scrollbarChanged)
    Q_PROPERTY(bool bellRinging READ bellRinging NOTIFY bellChanged)
    Q_PROPERTY(bool bellBorderVisible READ bellBorderVisible NOTIFY bellChanged)

public:
    explicit TerminalPane(
        const LaunchOptions &options,
        QQuickItem *parent = nullptr,
        std::optional<TerminalSessionGeometry> initialGeometry = std::nullopt,
        TerminalSessionStartMode startMode =
            TerminalSessionStartMode::Immediate,
        std::shared_ptr<InitialSessionCoordinator>
            initialSessionCoordinator = {},
        std::optional<GhosttyKeybindProgram> keybindProgram = std::nullopt);
    ~TerminalPane() override;

    QString title() const;
    // The application-visible surface title intentionally excludes the Qt
    // launch fallback and any containing-tab override. Clipboard and prompt
    // actions consume this exact layer.
    [[nodiscard]] std::optional<QString> effectiveSurfaceTitle() const;
    [[nodiscard]] const std::optional<QString> &surfaceTitleOverride() const
    {
        return surfaceTitleOverride_;
    }
    QString currentDirectory() const;
    qreal fontPointSize() const;
    QStringList activeKeyTables() const;
    QString linkPreviewText() const;
    QRectF linkPreviewRect() const;
    bool searchUiActive() const { return searchUiActive_; }
    QString searchUiText() const { return searchUiText_; }
    QString searchMatchLabel() const { return searchMatchLabel_; }
    bool isRunning() const;
    bool hasActiveProcess() const;
    bool isReadOnly() const;
    bool resizeOverlayVisible() const { return resizeOverlayVisible_; }
    QString resizeOverlayText() const { return resizeOverlayText_; }
    QRectF resizeOverlayRect() const;
    bool scrollbarVisible() const { return scrollbarVisible_; }
    qreal scrollbarPosition() const { return scrollbarPosition_; }
    qreal scrollbarSize() const { return scrollbarSize_; }
    bool bellRinging() const { return bellRinging_; }
    bool bellTitleVisible() const
    {
        return bellRinging_ && options_.bellFeatures.title;
    }
    bool bellBorderVisible() const
    {
        return bellRinging_ && options_.bellFeatures.border;
    }
    LaunchOptions splitLaunchOptions(const LaunchOptions &base) const;
    LaunchOptions tabLaunchOptions(const LaunchOptions &base) const;
    LaunchOptions windowLaunchOptions(const LaunchOptions &base) const;
    void applyRuntimeOptions(const LaunchOptions &options);
    void applyRuntimeOptions(const LaunchOptions &options,
                             GhosttyKeybindProgram keybindProgram);
    [[nodiscard]] const GhosttyKeybindProgram &keybindProgram() const noexcept
    {
        return keybinds_.program();
    }
    // The workspace owns split topology; the pane owns actual focus and
    // search visibility, which complete Ghostty's dimming predicate.
    void setSplit(bool split);
    void beginShutdown();
    // ApplicationController arms only the first pane of a window that was
    // presented maximized/fullscreen. The actual start remains queued until
    // Qt reports a stable exposed viewport.
    [[nodiscard]] bool armDeferredSessionStart(
        std::function<QSizeF()> viewportSizeProvider = {});
    void setWorkspaceActionHandler(
        std::function<bool(WorkspaceActionRequest)> handler);
    // Dependency injection keeps external URL launches out of automated
    // tests while production defaults to QDesktopServices::openUrl.
    void setUrlOpener(std::function<bool(const QUrl &)> opener);

    void focusTerminal();
    void setSurfaceTitle(QString title);
    void setSurfaceTitleOverride(std::optional<QString> title);
    void copySelection();
    void pasteText(const QString &text);
    void confirmPaste(quint64 requestId);
    void cancelPaste(quint64 requestId);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    Q_INVOKABLE void setSearchUiText(const QString &text);
    Q_INVOKABLE void endSearchUi();
    Q_INVOKABLE void navigateSearch(int direction);
    Q_INVOKABLE void scrollbarMoveTo(qreal position);
    // Process-wide `all:`/`global:` dispatch reuses the same exact pane action
    // implementation as a focused local binding.
    bool executeConfiguredAction(QStringView action);
    bool executeConfiguredAction(const GhosttyConfiguredAction &action);

Q_SIGNALS:
    void activated(TerminalPane *pane);
    void titleChanged();
    void currentDirectoryChanged();
    void fontPointSizeChanged();
    void activeKeyTablesChanged();
    void linkPreviewChanged();
    void searchUiActiveChanged();
    void searchUiTextChanged();
    void searchMatchLabelChanged();
    void searchUiFocusRequested();
    void processStateChanged();
    void readOnlyChanged(bool readOnly);
    void resizeOverlayVisibleChanged();
    void resizeOverlayTextChanged();
    void resizeOverlayRectChanged();
    void scrollbarChanged();
    void bellChanged();
    void bellRang(TerminalPane *pane);
    void requestNewTab();
    void requestSplit(WorkspaceAction action);
    void requestClose();
    void requestCloseTab(CloseTabMode mode);
    void requestNavigate(int direction);
    void requestTabChange(int delta);
    void requestCloseWindow();
    void applicationActionRequested(ApplicationAction action);
    void windowNavigationRequested(WindowNavigationAction action);
    void broadActionsRequested(const GhosttyCompiledActionChain &actions);
    void unsafePasteRequested(quint64 requestId, const QString &text,
                              TerminalPane *pane);
    void sessionEnded(TerminalPane *pane, int exitCode, int signalNumber);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *updateData) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    friend class GhosttyApplicationKeybindings;
    friend class TerminalWorkspace;

    enum class KeyHandling {
        PassThrough,
        ConsumePress,
        ConsumePressAndRelease,
    };

    using ConfiguredActionCompletion =
        std::function<void(TerminalActionExecutionResult)>;
    struct PendingTerminalActionCompletion {
        ConfiguredActionCompletion completion;
        quint64 epoch = 0;
    };
    struct DeferredKeyInput {
        KeyEventSnapshot event;
        quint64 focusEpoch = 0;
    };
    using DeferredPaneInput =
        std::variant<DeferredKeyInput, TerminalInputMethodInput>;

    struct PendingLocalActionChain {
        GhosttyCompiledActionChain chain;
        qsizetype nextEntry = 0;
        bool performed = false;
        quint64 sequenceToken = 0;
        TerminalKeyInput currentInput;
        quint64 keyIdentity = 0;
        quint64 keyFocusEpoch = 0;
        bool consumed = true;
        bool performable = false;
        bool ownsKeyDeferral = false;
        bool startingAction = false;
        std::optional<TerminalActionExecutionResult> earlyResult;
    };

    [[nodiscard]] bool updateMetrics();
    [[nodiscard]] bool updateMetrics(
        const TerminalTypography &typography, qreal pointSize);
    void updateTerminalSize();
    void noteTerminalGridSize(const TerminalSessionGeometry &geometry);
    void scheduleResizeOverlay();
    void cancelPendingResizeOverlay();
    void showPendingResizeOverlay();
    void hideResizeOverlay();
    void restartResizeOverlayTimer();
    [[nodiscard]] std::optional<TerminalSessionGeometry>
    currentSessionGeometry(
        std::optional<QSizeF> viewportSize = std::nullopt) const;
    void watchWindow(QQuickWindow *quickWindow);
    void disconnectDeferredSessionWindowSignals();
    void scheduleDeferredSessionStart();
    void tryDeferredSessionStart();
    void markTextRowsChangedLocked(const TerminalUpdate &update);
    void syncCursorBlink(bool resetPhase);
    void setFontPointSize(qreal points);
    void beginKeyEventDeferral() noexcept;
    void endKeyEventDeferral();
    void beginKeyEventDispatch() noexcept;
    void endKeyEventDispatch();
    [[nodiscard]] bool deferKeyEventIfNeeded(const QKeyEvent &event);
    void deferKeyEvent(const QKeyEvent &event);
    void drainDeferredKeyEvents();
    KeyHandling handleShortcut(
        QKeyEvent *event, const QPointer<TerminalPane> &guard);
    KeyHandling handleConfiguredShortcut(
        QKeyEvent *event, const QPointer<TerminalPane> &guard);
    [[nodiscard]] bool resolveActiveSequence(
        TerminalSequenceResolution resolution,
        std::optional<TerminalKeyInput> current = std::nullopt);
    [[nodiscard]] bool resolveSequenceToken(
        quint64 token, TerminalSequenceResolution resolution,
        std::optional<TerminalKeyInput> current = std::nullopt);
    [[nodiscard]] bool resolveExecutingSequence(
        TerminalSequenceResolution resolution,
        std::optional<TerminalKeyInput> current = std::nullopt);
    [[nodiscard]] bool performConfiguredAction(
        const GhosttyConfiguredAction &action);
    [[nodiscard]] TerminalActionExecutionStart startConfiguredAction(
        const GhosttyConfiguredAction &action,
        ConfiguredActionCompletion completion);
    [[nodiscard]] bool commitConfiguredActionResult(
        const TerminalActionExecutionResult &result);
    [[nodiscard]] quint64 nextTerminalActionRequestId();
    void advanceTerminalActionEpoch();
    void failStaleTerminalActionCompletions();
    void handleTerminalActionResult(const TerminalActionResult &result);
    [[nodiscard]] std::optional<KeyHandling> continueLocalActionChain(
        const std::shared_ptr<PendingLocalActionChain> &chain);
    [[nodiscard]] KeyHandling finishLocalActionChain(
        PendingLocalActionChain &chain, bool delayed);
    [[nodiscard]] bool performPaneAction(const GhosttyPaneAction &action);
    [[nodiscard]] bool performWorkspaceAction(WorkspaceActionRequest request);
    int viewportPageRows() const;
    void beginLocalSelection(const QPointF &position, int clickCount,
                             Qt::KeyboardModifiers modifiers);
    void sendMouse(const QPointF &position, TerminalMouseInput::Action action,
                   Qt::MouseButton button, Qt::MouseButtons buttons,
                   Qt::KeyboardModifiers modifiers);
    QPoint cellAt(const QPointF &position) const;
    std::optional<QPoint> hoverCellAt(const QPointF &position) const;
    int normalizedMouseButton(Qt::MouseButton button) const;
    Qt::KeyboardModifiers effectivePointerModifiers(
        Qt::KeyboardModifiers modifiers) const;
    bool hyperlinkModifiersMatch(Qt::KeyboardModifiers modifiers) const;
    void updateHyperlinkHover(const QPointF &position,
                              Qt::KeyboardModifiers modifiers);
    [[nodiscard]] std::optional<QByteArray> hoveredUrlForCopy() const;
    void updateHyperlinkModifiers(Qt::KeyboardModifiers modifiers);
    void recomputeHyperlinkHover();
    void refreshHyperlinkHover();
    void refreshLinkPreview();
    void reconcileReleasedLinkPreview(bool wasPointerCaptured,
                                      bool forceRequery = false);
    void clearHyperlinkDecoration();
    void clearHyperlinkHover();
    void cancelHyperlinkPress();
    void cancelPendingHyperlinkActivation();
    bool hyperlinkCellCandidate(const QPoint &cell,
                                quint64 *contentRevision = nullptr) const;
    void handleHyperlinkResult(quint64 contentRevision,
                               TerminalHyperlinkState state,
                               TerminalLinkKind kind,
                               const QByteArray &uri,
                               const QPoint &targetCell,
                               const QVector<QPoint> &matchingCells);
    void handleHyperlinkActivation(quint64 contentRevision,
                                   TerminalLinkKind kind,
                                   const QByteArray &uri);
    QUrl hyperlinkUrl(const QByteArray &uri, TerminalLinkKind kind) const;
    void startSearchUi();
    void startSearchUiWithSelection(const QString &text);
    void setSearchUiActive(bool active);
    void handleSearchUpdate(const TerminalSearchUpdate &searchUpdate);
    void installSearchDecorationsLocked(
        const TerminalSearchUpdate &searchUpdate);
    void clearPendingSearchUpdateLocked();
    void clearSearchDecorationsLocked();
    void updateScrollbarState();
    void setBellRinging(bool ringing);

    LaunchOptions options_;
    // Mirrored separately so the render thread can take a value-only snapshot
    // under renderMutex_ while live configuration updates options_.
    TerminalAppearance appearance_;
    SplitAppearance splitAppearance_;
    GhosttyKeybindState keybinds_;
    RevisionCounter runtimeOptionsRevision_;
    TerminalController *controller_ = nullptr;
    std::optional<QString> surfaceTitleOverride_;
    TerminalCellMetrics metrics_;
    double defaultFontPointSize_ = 12.0;
    mutable QMutex renderMutex_;
    TerminalFrame frame_;
    // Persistent generations preserve the union of row changes when Qt
    // coalesces several GUI updates before one scene-graph synchronization.
    QVector<quint64> textRowEpochs_;
    quint64 textRowEpoch_ = 0;
    bool hasFrame_ = false;
    // Mirrors the row count requested from the worker. While a resize is in
    // flight, stale frames must not make page actions use the previous height.
    int terminalRows_ = 24;
    bool terminalResizePending_ = false;
    QString preedit_;
    QString statusMessage_;
    bool selecting_ = false;
    bool manuallyZoomed_ = false;
    bool cursorBlinkOn_ = true;
    QTimer *cursorTimer_ = nullptr;
    QChronoTimer *resizeOverlayTimer_ = nullptr;
    std::chrono::steady_clock::time_point
        resizeOverlayStartupSuppressionEnds_;
    std::optional<QSize> resizeOverlayGrid_;
    QString resizeOverlayText_;
    bool resizeOverlayVisible_ = false;
    bool resizeOverlayUpdateScheduled_ = false;
    quint64 resizeOverlayUpdateGeneration_ = 0;
    bool resizeOverlayShuttingDown_ = false;
    bool scrollbarVisible_ = false;
    qreal scrollbarPosition_ = 0.0;
    qreal scrollbarSize_ = 1.0;
    std::optional<quint64> pendingScrollbarRow_;
    bool bellRinging_ = false;
    QMetaObject::Connection itemWindowConnection_;
    QMetaObject::Connection windowActiveConnection_;
    QMetaObject::Connection windowScreenConnection_;
    QMetaObject::Connection windowVisibilityConnection_;
    QMetaObject::Connection windowStateConnection_;
    QMetaObject::Connection windowFrameSwappedConnection_;
    QPointer<QQuickWindow> observedWindow_;
    TerminalSessionStartMode sessionStartMode_ =
        TerminalSessionStartMode::Immediate;
    bool deferredSessionStartArmed_ = false;
    bool deferredSessionStartCheckQueued_ = false;
    std::optional<TerminalSessionGeometry> deferredSessionStartCandidate_;
    std::function<QSizeF()> deferredSessionViewportSizeProvider_;
    quint64 deferredSessionPresentedFrame_ = 0;
    quint64 deferredSessionCandidateFrame_ = 0;
    QSet<quint64> consumedKeys_;
    quint64 keyFocusEpoch_ = 0;
    std::deque<DeferredPaneInput> deferredInputs_;
    int keyEventDeferralDepth_ = 0;
    int keyEventDispatchDepth_ = 0;
    bool drainingDeferredKeyEvents_ = false;
    const QKeyEvent *replayingDeferredKeyEvent_ = nullptr;
    quint64 activeSequenceToken_ = 0;
    QVector<quint64> executingSequenceTokens_;
    quint64 nextTerminalActionRequestId_ = 0;
    quint64 terminalActionEpoch_ = 1;
    bool terminalActionsAccepted_ = true;
    QHash<quint64, PendingTerminalActionCompletion>
        pendingTerminalActionCompletions_;
    bool hoverInside_ = false;
    QPointF hoverPosition_;
    QPoint hoverCell_{-1, -1};
    Qt::KeyboardModifiers keyboardModifiers_ = Qt::NoModifier;
    Qt::KeyboardModifiers hoverModifiers_ = Qt::NoModifier;
    QPoint hyperlinkQueryCell_{-1, -1};
    bool hyperlinkQueryPending_ = false;
    bool hyperlinkLeaseActive_ = false;
    bool hyperlinkQueryRejected_ = false;
    TerminalLinkKind hoveredLinkKind_ = TerminalLinkKind::Osc8;
    QByteArray hoveredHyperlinkUri_;
    QPoint hoveredHyperlinkCell_{-1, -1};
    QSet<int> hoveredHyperlinkCellIndexes_;
    int hoveredHyperlinkColumns_ = 0;
    int hoveredHyperlinkRows_ = 0;
    QString linkPreviewText_;
    QRectF linkPreviewRect_;
    QRectF linkPreviewGuardRect_;
    bool linkPreviewPointerCaptured_ = false;
    bool hyperlinkPressArmed_ = false;
    bool hyperlinkPressDragged_ = false;
    QPointF hyperlinkPressPosition_;
    QPoint hyperlinkPressCell_{-1, -1};
    TerminalLinkKind hyperlinkPressKind_ = TerminalLinkKind::Osc8;
    QByteArray hyperlinkPressUri_;
    quint64 hyperlinkPressRequestId_ = 0;
    QByteArray pendingActivationUri_;
    TerminalLinkKind pendingActivationKind_ = TerminalLinkKind::Osc8;
    quint64 pendingActivationRequestId_ = 0;
    std::function<bool(const QUrl &)> urlOpener_;
    std::shared_ptr<std::function<bool(WorkspaceActionRequest)>>
        workspaceActionHandler_;
    bool searchUiActive_ = false;
    bool split_ = false;
    bool searchEngineActive_ = false;
    QString searchUiText_;
    QString searchMatchLabel_ = QStringLiteral("0/0");
    std::optional<TerminalSearchUpdate> pendingSearchUpdate_;
    QBitArray searchCandidateCellMask_;
    QBitArray searchSelectedCellMask_;
    quint64 searchDecorationRevision_ = 0;
    int searchDecorationColumns_ = 0;
    int searchDecorationRows_ = 0;
};
