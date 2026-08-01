#pragma once

#include "terminal_inspector_snapshot.h"

#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

class QTimer;
class TerminalPane;

// A short-lived, GUI-thread view of one terminal surface. The model exists
// only while that surface's inspector is open; it never retains libghostty
// handles or worker-thread state.
class TerminalInspectorModel final : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QVariantMap snapshot READ snapshot NOTIFY snapshotChanged)

public:
    explicit TerminalInspectorModel(TerminalPane *pane);

    [[nodiscard]] QVariantMap snapshot() const { return snapshot_; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void beginCellPick();
    Q_INVOKABLE void cancelCellPick();
    Q_INVOKABLE void close();
    void deactivate();

Q_SIGNALS:
    void snapshotChanged();

private:
    void rebuildSnapshot();

    QPointer<TerminalPane> pane_;
    QVariantMap snapshot_;
    QTimer *refreshTimer_ = nullptr;
    TerminalInspectorSnapshot terminalSnapshot_;
    quint64 pendingTerminalRequestId_ = 0;
    TerminalInspectorCellSnapshot cellSnapshot_;
    quint64 pendingCellRequestId_ = 0;
    bool cellHasResult_ = false;
    bool active_ = true;
};
