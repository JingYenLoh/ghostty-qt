#pragma once

#include "ghostty_keybind_set.h"
#include "launch_options.h"
#include "terminal_types.h"
#include "workspace_action.h"

#include <QByteArray>
#include <QFont>
#include <QMetaObject>
#include <QMutex>
#include <QPoint>
#include <QQuickItem>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <optional>

class QFocusEvent;
class QHoverEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QSGNode;
class QTimer;
class QWheelEvent;
class TerminalController;
struct TerminalFontSizeRequest;
struct TerminalKeyTableRequest;

class TerminalPane final : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString currentDirectory READ currentDirectory NOTIFY currentDirectoryChanged)
    Q_PROPERTY(qreal fontPointSize READ fontPointSize NOTIFY fontPointSizeChanged)
    Q_PROPERTY(QStringList activeKeyTables READ activeKeyTables NOTIFY activeKeyTablesChanged)
    Q_PROPERTY(QString linkPreviewText READ linkPreviewText NOTIFY linkPreviewChanged)
    Q_PROPERTY(QRectF linkPreviewRect READ linkPreviewRect NOTIFY linkPreviewChanged)
    Q_PROPERTY(bool searchUiActive READ searchUiActive NOTIFY searchUiActiveChanged)
    Q_PROPERTY(QString searchUiText READ searchUiText NOTIFY searchUiTextChanged)
    Q_PROPERTY(QString searchMatchLabel READ searchMatchLabel NOTIFY searchMatchLabelChanged)

public:
    explicit TerminalPane(const LaunchOptions &options, QQuickItem *parent = nullptr);
    ~TerminalPane() override;

    QString title() const;
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
    LaunchOptions splitLaunchOptions(const LaunchOptions &base) const;
    LaunchOptions tabLaunchOptions(const LaunchOptions &base) const;
    void applyRuntimeOptions(const LaunchOptions &options);
    // The workspace owns split topology; the pane owns actual focus and
    // search visibility, which complete Ghostty's dimming predicate.
    void setSplit(bool split);
    void beginShutdown();
    void setWorkspaceActionHandler(
        std::function<bool(WorkspaceActionRequest)> handler);
    // Dependency injection keeps external URL launches out of automated
    // tests while production defaults to QDesktopServices::openUrl.
    void setUrlOpener(std::function<bool(const QUrl &)> opener);

    void focusTerminal();
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
    // Process-wide `all:`/`global:` dispatch reuses the same exact pane action
    // implementation as a focused local binding.
    bool executeConfiguredAction(QStringView action);

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
    void requestNewTab();
    void requestSplit(WorkspaceAction action);
    void requestClose();
    void requestCloseTab();
    void requestNavigate(int direction);
    void requestTabChange(int delta);
    void requestQuit();
    void requestConfigReload();
    void broadActionsRequested(const QStringList &actions);
    void unsafePasteRequested(quint64 requestId, const QString &text,
                              TerminalPane *pane);
    void sessionEnded(TerminalPane *pane, int exitCode, int signalNumber);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *updateData) override;
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
    enum class KeyHandling {
        PassThrough,
        ConsumePress,
        ConsumePressAndRelease,
    };

    void updateMetrics();
    void updateTerminalSize();
    void markTextRowsChangedLocked(const TerminalUpdate &update);
    void syncCursorBlink(bool resetPhase);
    void setFontPointSize(qreal points);
    KeyHandling handleShortcut(QKeyEvent *event);
    KeyHandling handleConfiguredShortcut(QKeyEvent *event);
    bool canExecuteConfiguredAction(QStringView action) const;
    int viewportPageRows() const;
    void applyFontSizeRequest(const TerminalFontSizeRequest &request);
    [[nodiscard]] bool canApplyKeyTableRequest(
        const TerminalKeyTableRequest &request) const;
    [[nodiscard]] bool applyKeyTableRequest(
        const TerminalKeyTableRequest &request);
    void beginLocalSelection(const QPointF &position, int clickCount,
                             Qt::KeyboardModifiers modifiers);
    void sendMouse(const QPointF &position, TerminalMouseInput::Action action,
                   Qt::MouseButton button, Qt::MouseButtons buttons,
                   Qt::KeyboardModifiers modifiers);
    Qt::MouseButtons reportedMouseButtons(Qt::MouseButtons buttons) const;
    QPoint cellAt(const QPointF &position) const;
    std::optional<QPoint> hoverCellAt(const QPointF &position) const;
    int normalizedMouseButton(Qt::MouseButton button) const;
    Qt::KeyboardModifiers effectivePointerModifiers(
        Qt::KeyboardModifiers modifiers) const;
    bool hyperlinkModifiersMatch(Qt::KeyboardModifiers modifiers) const;
    void updateHyperlinkHover(const QPointF &position,
                              Qt::KeyboardModifiers modifiers);
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
    void setSearchUiActive(bool active);
    void handleSearchUpdate(const TerminalSearchUpdate &searchUpdate);
    void installSearchDecorationsLocked(
        const TerminalSearchUpdate &searchUpdate);
    void clearPendingSearchUpdateLocked();
    void clearSearchDecorationsLocked();

    LaunchOptions options_;
    // Mirrored separately so the render thread can take a value-only snapshot
    // under renderMutex_ while live configuration updates options_.
    TerminalAppearance appearance_;
    SplitAppearance splitAppearance_;
    GhosttyKeybindSet keybinds_;
    TerminalController *controller_ = nullptr;
    QFont font_;
    qreal cellWidth_ = 8.0;
    qreal cellHeight_ = 16.0;
    qreal baseline_ = 13.0;
    qreal defaultFontPointSize_ = 12.0;
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
    QMetaObject::Connection windowActiveConnection_;
    QSet<quint64> consumedKeys_;
    quint64 activeSequenceToken_ = 0;
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
    QSet<Qt::MouseButton> mouseReportedPresses_;
    std::function<bool(const QUrl &)> urlOpener_;
    std::function<bool(WorkspaceActionRequest)> workspaceActionHandler_;
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
