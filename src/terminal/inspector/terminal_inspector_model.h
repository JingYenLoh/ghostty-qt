#pragma once

#include "terminal/inspector/terminal_inspector_event_model.h"
#include "terminal/inspector/terminal_inspector_snapshot.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

class QTimer;
class TerminalPane;
struct TerminalKeyInput;
struct TerminalUpdate;

// A short-lived, GUI-thread view of one terminal surface. The model exists
// only while that surface's inspector is open; it never retains libghostty
// handles or worker-thread state.
class TerminalInspectorModel final : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QVariantMap snapshot READ snapshot NOTIFY snapshotChanged)
    Q_PROPERTY(QAbstractItemModel *eventModel READ eventModel CONSTANT)

public:
    explicit TerminalInspectorModel(TerminalPane *pane);

    [[nodiscard]] QVariantMap snapshot() const { return snapshot_; }
    [[nodiscard]] QAbstractItemModel *eventModel() const { return eventModel_; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void beginCellPick();
    Q_INVOKABLE void cancelCellPick();
    Q_INVOKABLE void close();
    void deactivate();

Q_SIGNALS:
    void snapshotChanged();

private:
    struct PendingTerminalEvent {
        int updates = 0;
        int columns = 0;
        int rows = 0;
        qsizetype dirtyRows = 0;
        int cursorColumn = 0;
        int cursorRow = 0;
        quint64 firstRevision = 0;
        quint64 lastRevision = 0;
        bool fullFrame = false;
        bool colorsChanged = false;
        bool cursorChanged = false;
        bool scrollbarChanged = false;
        bool kittyGraphicsChanged = false;
        bool resetCursorBlink = false;
    };

    void rebuildSnapshot();
    void appendEvent(TerminalInspectorEventModel::Category category,
                     QString kind, QString summary, QString details = {},
                     quint64 traceId = 0);
    [[nodiscard]]
    bool acceptsKeyboardTraceInput(const TerminalKeyInput &input) const;
    [[nodiscard]] bool skipEventWhilePaused();
    void recordTerminalUpdate(const TerminalUpdate &update);
    void flushPendingTerminalEvent();
    void clearPendingTerminalEvent();

    QPointer<TerminalPane> pane_;
    QVariantMap snapshot_;
    TerminalInspectorEventModel *eventModel_ = nullptr;
    QTimer *refreshTimer_ = nullptr;
    QTimer *terminalEventTimer_ = nullptr;
    PendingTerminalEvent pendingTerminalEvent_;
    TerminalInspectorSnapshot terminalSnapshot_;
    quint64 pendingTerminalRequestId_ = 0;
    TerminalInspectorCellSnapshot cellSnapshot_;
    quint64 pendingCellRequestId_ = 0;
    bool cellHasResult_ = false;
    bool active_ = true;
};
