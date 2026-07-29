#pragma once

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE

#include "terminal_cell_metrics.h"
#include "terminal_session_options.h"

#include <QColor>
#include <QRectF>
#include <QVector>
#include <QtGlobal>

#include <array>
#include <optional>

class TerminalPane;

struct TerminalPaneRenderProbeSnapshot {
    // Captured before the controller starts so tests can verify the one-shot
    // first-pane launch contract independently of scene-graph rendering.
    std::optional<TerminalSessionGeometry> initialGeometry;
    quint64 paintSerial = 0;
    quint64 rootSerial = 0;
    quint64 unfocusedSplitOverlaySerial = 0;
    quint64 startingTextNodeSerial = 0;
    quint64 overlayTextNodeSerial = 0;
    quint64 paneOverlayTextNodeSerial = 0;
    // Cumulative layout rebuilds for retained grid and pane overlay nodes.
    quint64 overlayTextBuildCount = 0;
    quint64 paneOverlayTextBuildCount = 0;
    quint64 startingTextBuildCount = 0;
    quint64 backgroundImageAssetSerial = 0;
    QRectF backgroundImageRect;
    QRectF backgroundImageSourceRect;
    QVector<QRectF> backdropBaseRects;
    QRectF unfocusedSplitOverlayRect;
    QColor unfocusedSplitOverlayColor;
    QVector<quint64> rowNodeSerials;
    // Cumulative rebuilds by visible row for the current scene-graph root.
    QVector<quint64> rowBuildCounts;
    TerminalCellMetrics metrics;
    std::array<QFont, terminalEnumIndex(TerminalFontRole::Count)> renderFonts;
    std::array<quint64, terminalEnumIndex(TerminalFontRole::Count)>
        fontRoleCellCounts{};
    QColor baseBackground;
    // Row-major effective cell layers. A valid zero-alpha color represents a
    // default cell that contributes no second fill over baseBackground.
    QVector<QColor> cellBackgrounds;
    QVector<QColor> glyphForegrounds;
    QVector<QColor> decorationForegrounds;
    QVector<QColor> underlineColors;
    QColor cursorColor;
    QVector<QRectF> underlineRects;
    QVector<QRectF> strikethroughRects;
    QVector<QRectF> overlineRects;
    QVector<QRectF> cursorRects;
};

[[nodiscard]] TerminalPaneRenderProbeSnapshot
terminalPaneRenderProbe(const TerminalPane *pane);

[[nodiscard]] QColor terminalMinimumContrastColorForTest(
    const QColor &foreground, const QColor &cellBackground,
    const QColor &globalBackground, double minimumContrast);

#endif
