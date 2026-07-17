#pragma once

#include "ghostty_keybind_set.h"
#include "launch_options.h"
#include "terminal_types.h"
#include "workspace_action.h"

#include <QByteArray>
#include <QFont>
#include <QMutex>
#include <QPoint>
#include <QQuickItem>
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

class TerminalPane final : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString currentDirectory READ currentDirectory NOTIFY currentDirectoryChanged)
    Q_PROPERTY(qreal fontPointSize READ fontPointSize NOTIFY fontPointSizeChanged)
    Q_PROPERTY(QStringList activeKeyTables READ activeKeyTables NOTIFY activeKeyTablesChanged)

public:
    explicit TerminalPane(const LaunchOptions &options, QQuickItem *parent = nullptr);
    ~TerminalPane() override;

    QString title() const;
    QString currentDirectory() const;
    qreal fontPointSize() const;
    QStringList activeKeyTables() const;
    bool isRunning() const;
    bool hasActiveProcess() const;
    LaunchOptions splitLaunchOptions() const;
    void applyRuntimeOptions(const LaunchOptions &options);
    void beginShutdown();
    void setWorkspaceActionHandler(
        std::function<bool(WorkspaceActionRequest)> handler);
    // Dependency injection keeps external URL launches out of automated
    // tests while production defaults to QDesktopServices::openUrl.
    void setUrlOpener(std::function<bool(const QUrl &)> opener);

    void focusTerminal();
    void copySelection();
    void pasteText(const QString &text);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    // Process-wide `all:`/`global:` dispatch reuses the same exact pane action
    // implementation as a focused local binding.
    bool executeConfiguredAction(QStringView action);

Q_SIGNALS:
    void activated(TerminalPane *pane);
    void titleChanged();
    void currentDirectoryChanged();
    void fontPointSizeChanged();
    void activeKeyTablesChanged();
    void processStateChanged();
    void requestNewTab();
    void requestSplit(int orientation);
    void requestClose();
    void requestCloseTab();
    void requestNavigate(int direction);
    void requestTabChange(int delta);
    void requestQuit();
    void requestConfigReload();
    void broadActionsRequested(const QStringList &actions);
    void unsafePasteRequested(const QString &text, TerminalPane *pane);
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
    void resetCursorBlink();
    void setFontPointSize(qreal points);
    KeyHandling handleShortcut(QKeyEvent *event);
    KeyHandling handleConfiguredShortcut(QKeyEvent *event);
    bool canExecuteConfiguredAction(QStringView action) const;
    int viewportPageRows() const;
    void adjustZoom(qreal delta);
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
    void clearHyperlinkDecoration();
    void clearHyperlinkHover();
    void cancelHyperlinkPress();
    void cancelPendingHyperlinkActivation();
    bool hyperlinkCellCandidate(const QPoint &cell,
                                quint64 *contentRevision = nullptr) const;
    void handleHyperlinkResult(quint64 contentRevision,
                               TerminalHyperlinkState state,
                               const QByteArray &uri,
                               const QPoint &targetCell,
                               const QVector<QPoint> &matchingCells);
    void handleHyperlinkActivation(quint64 contentRevision,
                                   const QByteArray &uri);
    QUrl hyperlinkUrl(const QByteArray &uri) const;

    LaunchOptions options_;
    // Mirrored separately so the render thread can take a value-only snapshot
    // under renderMutex_ while live configuration updates options_.
    TerminalAppearance appearance_;
    GhosttyKeybindSet keybinds_;
    TerminalController *controller_ = nullptr;
    QFont font_;
    qreal cellWidth_ = 8.0;
    qreal cellHeight_ = 16.0;
    qreal baseline_ = 13.0;
    qreal defaultFontPointSize_ = 12.0;
    mutable QMutex renderMutex_;
    TerminalFrame frame_;
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
    QByteArray hoveredHyperlinkUri_;
    QPoint hoveredHyperlinkCell_{-1, -1};
    QSet<int> hoveredHyperlinkCellIndexes_;
    int hoveredHyperlinkColumns_ = 0;
    int hoveredHyperlinkRows_ = 0;
    bool hyperlinkPressArmed_ = false;
    bool hyperlinkPressDragged_ = false;
    QPointF hyperlinkPressPosition_;
    QPoint hyperlinkPressCell_{-1, -1};
    QByteArray hyperlinkPressUri_;
    quint64 hyperlinkPressRequestId_ = 0;
    QByteArray pendingActivationUri_;
    quint64 pendingActivationRequestId_ = 0;
    QSet<Qt::MouseButton> mouseReportedPresses_;
    std::function<bool(const QUrl &)> urlOpener_;
    std::function<bool(WorkspaceActionRequest)> workspaceActionHandler_;
};
