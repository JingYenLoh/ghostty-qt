#include "terminal_kitty_graphics.h"

#include <algorithm>
#include <cmath>

std::optional<TerminalKittyGraphicsRenderPlacement>
terminalKittyGraphicsRenderPlacement(
    const TerminalKittyGraphicsPlacement &placement, const QSizeF &cellSize,
    const QSizeF &terminalCellPixelSize, const QRectF &gridViewport) noexcept
{
    if (placement.image == nullptr || placement.image->straightRgba.isNull()
        || placement.image->straightRgba.format() != QImage::Format_RGBA8888
        || !std::isfinite(cellSize.width()) || !std::isfinite(cellSize.height())
        || cellSize.width() <= 0.0 || cellSize.height() <= 0.0
        || !std::isfinite(terminalCellPixelSize.width())
        || !std::isfinite(terminalCellPixelSize.height())
        || terminalCellPixelSize.width() <= 0.0
        || terminalCellPixelSize.height() <= 0.0 || !gridViewport.isValid()
        || gridViewport.isEmpty() || placement.destinationWidthPixels == 0
        || placement.destinationHeightPixels == 0 || placement.sourceWidth == 0
        || placement.sourceHeight == 0) {
        return std::nullopt;
    }

    const QSize imageSize = placement.image->straightRgba.size();
    const quint64 sourceRight =
        static_cast<quint64>(placement.sourceX) + placement.sourceWidth;
    const quint64 sourceBottom =
        static_cast<quint64>(placement.sourceY) + placement.sourceHeight;
    if (sourceRight > static_cast<quint64>(imageSize.width())
        || sourceBottom > static_cast<quint64>(imageSize.height())) {
        return std::nullopt;
    }

    const qreal horizontalPixelScale =
        cellSize.width() / terminalCellPixelSize.width();
    const qreal verticalPixelScale =
        cellSize.height() / terminalCellPixelSize.height();
    const QRectF destination{
        static_cast<qreal>(placement.viewportColumn) * cellSize.width()
            + static_cast<qreal>(placement.xOffsetPixels)
                * horizontalPixelScale,
        static_cast<qreal>(placement.viewportRow) * cellSize.height()
            + static_cast<qreal>(placement.yOffsetPixels) * verticalPixelScale,
        static_cast<qreal>(placement.destinationWidthPixels)
            * horizontalPixelScale,
        static_cast<qreal>(placement.destinationHeightPixels)
            * verticalPixelScale,
    };
    const QRectF visible = destination.intersected(gridViewport);
    if (visible.isEmpty()) {
        return std::nullopt;
    }

    const qreal horizontalScale =
        static_cast<qreal>(placement.sourceWidth) / destination.width();
    const qreal verticalScale =
        static_cast<qreal>(placement.sourceHeight) / destination.height();
    const QRectF source{
        static_cast<qreal>(placement.sourceX)
            + (visible.left() - destination.left()) * horizontalScale,
        static_cast<qreal>(placement.sourceY)
            + (visible.top() - destination.top()) * verticalScale,
        visible.width() * horizontalScale,
        visible.height() * verticalScale,
    };

    return TerminalKittyGraphicsRenderPlacement{
        .image = placement.image,
        .placementId = placement.placementId,
        .z = placement.z,
        .layer = placement.layer,
        .destination = visible,
        .source = source,
    };
}
