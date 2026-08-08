#pragma once

#ifdef GHOSTTY_QT_RENDER_TEST_PROBE

#include "terminal_alpha_blending.h"
#include "terminal_cell_metrics.h"
#include "terminal_kitty_graphics.h"
#include "terminal_session_options.h"

#include <QColor>
#include <QRectF>
#include <QVector>
#include <QtGlobal>

#include <array>
#include <optional>

class TerminalPane;

enum class TerminalPaneRenderLayer : quint8 {
    KittyBelowBackground,
    CellBackground,
    KittyBelowText,
    CursorBackground,
    CellForeground,
    KittyAboveText,
    TerminalOverlay,
};

struct TerminalPaneRenderProbeSnapshot {
    // Captured before the controller starts so tests can verify the one-shot
    // first-pane launch contract independently of scene-graph rendering.
    std::optional<TerminalSessionGeometry> initialGeometry;
    quint64 paintSerial = 0;
    quint64 rootSerial = 0;
    TerminalAlphaBlending requestedAlphaBlending =
        TerminalAlphaBlending::LinearCorrected;
    TerminalAlphaBlending effectiveAlphaBlending =
        TerminalAlphaBlending::Native;
    quint64 unfocusedSplitOverlaySerial = 0;
    quint64 startingTextNodeSerial = 0;
    quint64 overlayTextNodeSerial = 0;
    quint64 paneOverlayTextNodeSerial = 0;
    quint64 kittyBelowBackgroundNodeSerial = 0;
    quint64 kittyBelowTextNodeSerial = 0;
    quint64 kittyAboveTextNodeSerial = 0;
    QVector<TerminalPaneRenderLayer> renderLayerOrder;
    // Cumulative layout rebuilds for retained grid and pane overlay nodes.
    quint64 overlayTextBuildCount = 0;
    quint64 paneOverlayTextBuildCount = 0;
    quint64 startingTextBuildCount = 0;
    quint64 backgroundImageAssetSerial = 0;
    QRectF backgroundImageRect;
    QRectF backgroundImageSourceRect;
    quint64 kittyGraphicsTextureUploadCount = 0;
    quint64 kittyGraphicsNodeCreationCount = 0;
    quint64 kittyGraphicsNodeDeletionCount = 0;
    quint64 kittyGraphicsGeometryWriteCount = 0;
    quint64 kittyGraphicsMaterialAssignmentCount = 0;
    quint64 kittyGraphicsTextureSetEvictionCount = 0;
    qsizetype kittyGraphicsTextureCount = 0;
    quint64 kittyGraphicsTextureBytes = 0;
    QVector<QRectF> kittyGraphicsDestinations;
    QVector<QRectF> kittyGraphicsSources;
    QVector<TerminalKittyGraphicsLayer> kittyGraphicsLayers;
    QVector<QRectF> backdropBaseRects;
    QRectF unfocusedSplitOverlayRect;
    QColor unfocusedSplitOverlayColor;
    // Zero means that the row has never required a native QSGTextNode since
    // its current row topology was created.
    QVector<quint64> rowNodeSerials;
    QVector<quint64> rowContainerSerials;
    QVector<quint64> rowGlyphBatchSerials;
    qsizetype nativeTextNodeCount = 0;
    // Cumulative rebuilds by visible row for the current scene-graph root.
    QVector<quint64> rowBuildCounts;
    // Cumulative cell-derived solid-plan rebuilds by visible row. Flattening a
    // cached row into its retained geometry batches does not increment this.
    QVector<quint64> rowSolidBuildCounts;
    // Actual retained geometry commits by visible row and painter layer.
    // Replanning a row to identical rectangles leaves these unchanged.
    bool retainedSolidRowGeometry = false;
    QVector<quint64> rowBackgroundGeometryCommitCounts;
    QVector<quint64> rowDecorationBeforeTextGeometryCommitCounts;
    QVector<quint64> rowDecorationAfterTextGeometryCommitCounts;
    // The software renderer keeps one flattened batch per painter layer.
    quint64 globalBackgroundGeometryCommitCount = 0;
    quint64 globalDecorationBeforeTextGeometryCommitCount = 0;
    quint64 globalDecorationAfterTextGeometryCommitCount = 0;
    quint64 solidCellVisitCount = 0;
    // Cumulative text-renderer work for the current scene-graph root. Native
    // submissions are runs delegated to Qt's text nodes; batched glyphs are
    // glyph instances emitted by the terminal-owned fast path.
    quint64 nativeTextSubmissionCount = 0;
    quint64 nativeTextCellCount = 0;
    quint64 batchedGlyphCount = 0;
    quint64 glyphBatchGeometryWriteCount = 0;
    quint64 glyphBatchNodeCreationCount = 0;
    quint64 glyphBatchAllocationCount = 0;
    quint64 glyphAtlasUploadCount = 0;
    // Identity of the current atlas resource; stable across compatible row
    // rebuilds and zero when no hardware atlas is resident.
    quint64 glyphAtlasSerial = 0;
    // Current terminal-owned glyph-atlas residency.
    qsizetype glyphAtlasEntryCount = 0;
    quint64 glyphAtlasBytes = 0;
    // Native QSGTextNode layout submissions in each row's latest rebuild.
    // A terminal-batched row is zero; a compatible native run counts once;
    // a rejected run counts once per exact-position fallback cell.
    QVector<quint64> rowLayoutCounts;
    QVector<quint64> rowFallbackCellCounts;
    QVector<quint64> rowBatchedGlyphCounts;
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

[[nodiscard]] bool
terminalPaneDelegatedPaintNodeTeardownForTest(TerminalPane *pane);

[[nodiscard]] QColor terminalMinimumContrastColorForTest(
    const QColor &foreground, const QColor &cellBackground,
    const QColor &globalBackground, double minimumContrast);

#endif
