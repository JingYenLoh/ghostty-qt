#pragma once

#include "terminal/model/terminal_session_options.h"

#include <QMarginsF>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QtGlobal>

#include <optional>

struct TerminalViewportSpec {
    QSizeF surfaceSize;
    QSizeF cellSize;
    qreal devicePixelRatio = 1.0;
    TerminalPaddingOptions padding;

    bool operator==(const TerminalViewportSpec &) const = default;
};

// One authoritative conversion result is shared by rendering, hit testing,
// libghostty, and the PTY. Device-pixel arithmetic selects the grid; logical
// rectangles merely project that exact result back into Qt's scene.
struct TerminalViewportLayout {
    TerminalSessionGeometry session;
    QRectF gridRect;

    bool operator==(const TerminalViewportLayout &) const = default;

    [[nodiscard]] QPoint clampedCellAt(QPointF surfacePosition) const noexcept;
    [[nodiscard]] std::optional<QPoint>
    strictCellAt(QPointF surfacePosition) const noexcept;
};

[[nodiscard]] QMarginsF
terminalExplicitPaddingMargins(const TerminalPaddingOptions &padding,
                               qreal devicePixelRatio) noexcept;

[[nodiscard]] std::optional<TerminalViewportLayout>
terminalViewportLayout(const TerminalViewportSpec &spec) noexcept;

// Convenience projection retained for construction paths that need only the
// worker payload. New pane code should keep the full layout.
[[nodiscard]] std::optional<TerminalSessionGeometry>
terminalSessionGeometryForViewport(qreal width, qreal height, qreal cellWidth,
                                   qreal cellHeight, qreal devicePixelRatio,
                                   const TerminalPaddingOptions &padding =
                                       TerminalPaddingOptions::none()) noexcept;
