#pragma once

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE

#include <QtGlobal>
#include <QVector>

class TerminalPane;

struct TerminalPaneRenderProbeSnapshot {
    quint64 paintSerial = 0;
    quint64 rootSerial = 0;
    QVector<quint64> rowNodeSerials;
    // Cumulative rebuilds by visible row for the current scene-graph root.
    QVector<quint64> rowBuildCounts;
};

[[nodiscard]] TerminalPaneRenderProbeSnapshot terminalPaneRenderProbe(
    const TerminalPane *pane);

#endif
