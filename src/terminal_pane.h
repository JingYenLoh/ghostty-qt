#pragma once

#include "launch_options.h"
#include "terminal_types.h"

#include <QFont>
#include <QMutex>
#include <QQuickItem>
#include <QSet>
#include <QString>

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

public:
    explicit TerminalPane(const LaunchOptions &options, QQuickItem *parent = nullptr);
    ~TerminalPane() override;

    QString title() const;
    QString currentDirectory() const;
    qreal fontPointSize() const;
    bool isRunning() const;
    LaunchOptions splitLaunchOptions() const;

    void focusTerminal();
    void copySelection();
    void pasteText(const QString &text);
    void zoomIn();
    void zoomOut();
    void resetZoom();

Q_SIGNALS:
    void activated(TerminalPane *pane);
    void titleChanged();
    void currentDirectoryChanged();
    void fontPointSizeChanged();
    void requestNewTab();
    void requestSplit(int orientation);
    void requestClose();
    void requestNavigate(int direction);
    void requestTabChange(int delta);
    void requestQuit();
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
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    void updateMetrics();
    void updateTerminalSize();
    void setFontPointSize(qreal points);
    bool handleShortcut(QKeyEvent *event);
    void beginLocalSelection(const QPointF &position, int clickCount,
                             Qt::KeyboardModifiers modifiers);
    void sendMouse(const QPointF &position, TerminalMouseInput::Action action,
                   Qt::MouseButton button, Qt::MouseButtons buttons,
                   Qt::KeyboardModifiers modifiers);
    QPoint cellAt(const QPointF &position) const;
    int normalizedMouseButton(Qt::MouseButton button) const;

    LaunchOptions options_;
    TerminalController *controller_ = nullptr;
    QFont font_;
    qreal cellWidth_ = 8.0;
    qreal cellHeight_ = 16.0;
    qreal baseline_ = 13.0;
    qreal defaultFontPointSize_ = 12.0;
    mutable QMutex renderMutex_;
    TerminalFrame frame_;
    bool hasFrame_ = false;
    QString preedit_;
    QString statusMessage_;
    bool selecting_ = false;
    bool cursorBlinkOn_ = true;
    QTimer *cursorTimer_ = nullptr;
    QSet<int> consumedKeys_;
};
