#include "terminal/core/terminal_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

bool positiveFinite(qreal value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

int boundedCellCount(int extent, int cellExtent) noexcept
{
    constexpr int maximum =
        static_cast<int>(std::numeric_limits<quint16>::max());
    return std::clamp(extent / cellExtent, 1, maximum);
}

int boundedDevicePixels(qreal logicalExtent, qreal devicePixelRatio) noexcept
{
    const long double pixels =
        std::round(static_cast<long double>(logicalExtent)
                   * static_cast<long double>(devicePixelRatio));
    constexpr int maximum = std::numeric_limits<int>::max();
    if (!std::isfinite(pixels) || pixels >= static_cast<long double>(maximum)) {
        return maximum;
    }
    return std::max(1, static_cast<int>(pixels));
}

int boundedPaddingPixels(quint32 points, qreal devicePixelRatio) noexcept
{
    // Linux uses the CSS reference DPI for point conversion: 96 / 72 = 4 / 3.
    const long double pixels =
        std::floor(static_cast<long double>(points)
                   * static_cast<long double>(devicePixelRatio) * 4.0L / 3.0L);
    constexpr int maximum = std::numeric_limits<int>::max();
    if (!std::isfinite(pixels) || pixels >= static_cast<long double>(maximum)) {
        return maximum;
    }
    return std::max(0, static_cast<int>(pixels));
}

int saturatingSubtract(int extent, int leading, int trailing) noexcept
{
    return static_cast<int>(
        std::max<qint64>(0, static_cast<qint64>(extent) - leading - trailing));
}

TerminalSessionGeometry::Padding
explicitPadding(const TerminalViewportSpec &spec) noexcept
{
    return {
        .top = boundedPaddingPixels(spec.padding.vertical.leadingPoints,
                                    spec.devicePixelRatio),
        .right = boundedPaddingPixels(spec.padding.horizontal.trailingPoints,
                                      spec.devicePixelRatio),
        .bottom = boundedPaddingPixels(spec.padding.vertical.trailingPoints,
                                       spec.devicePixelRatio),
        .left = boundedPaddingPixels(spec.padding.horizontal.leadingPoints,
                                     spec.devicePixelRatio),
    };
}

TerminalSessionGeometry::Padding
balancedPadding(const TerminalSessionGeometry &geometry,
                const TerminalSessionGeometry::Padding &explicitPixels,
                TerminalPaddingBalance mode) noexcept
{
    if (mode == TerminalPaddingBalance::Disabled) {
        return explicitPixels;
    }

    const qint64 gridWidth =
        static_cast<qint64>(geometry.columns) * geometry.cellWidthPixels;
    const qint64 gridHeight =
        static_cast<qint64>(geometry.rows) * geometry.cellHeightPixels;
    const int horizontal = static_cast<int>(std::clamp<qint64>(
        (static_cast<qint64>(geometry.surfaceWidthPixels) - gridWidth) / 2, 0,
        std::numeric_limits<int>::max()));
    int top = static_cast<int>(std::clamp<qint64>(
        (static_cast<qint64>(geometry.surfaceHeightPixels) - gridHeight) / 2, 0,
        std::numeric_limits<int>::max()));
    int bottom = top;

    if (mode == TerminalPaddingBalance::Balanced) {
        const qint64 cap = (static_cast<qint64>(explicitPixels.left)
                            + explicitPixels.right + geometry.cellWidthPixels)
            / 2;
        const int shift = static_cast<int>(
            std::max<qint64>(0, static_cast<qint64>(top) - cap));
        top -= shift;
        bottom = static_cast<int>(
            std::min<qint64>(std::numeric_limits<int>::max(),
                             static_cast<qint64>(bottom) + shift));
    }

    return {
        .top = top,
        .right = horizontal,
        .bottom = bottom,
        .left = horizontal,
    };
}

qreal logicalPixels(int physicalPixels, qreal devicePixelRatio) noexcept
{
    return static_cast<qreal>(physicalPixels) / devicePixelRatio;
}

int clampedCellIndex(qreal coordinate, int count) noexcept
{
    if (std::isnan(coordinate) || coordinate <= 0.0) return 0;
    if (!std::isfinite(coordinate) || coordinate >= static_cast<qreal>(count)) {
        return count - 1;
    }
    return static_cast<int>(std::floor(coordinate));
}

} // namespace

QMarginsF terminalExplicitPaddingMargins(const TerminalPaddingOptions &padding,
                                         qreal devicePixelRatio) noexcept
{
    if (!positiveFinite(devicePixelRatio)) return {};
    const TerminalSessionGeometry::Padding pixels = explicitPadding({
        .surfaceSize = {},
        .cellSize = {},
        .devicePixelRatio = devicePixelRatio,
        .padding = padding,
    });
    return {
        logicalPixels(pixels.left, devicePixelRatio),
        logicalPixels(pixels.top, devicePixelRatio),
        logicalPixels(pixels.right, devicePixelRatio),
        logicalPixels(pixels.bottom, devicePixelRatio),
    };
}

QPoint
TerminalViewportLayout::clampedCellAt(QPointF surfacePosition) const noexcept
{
    const qreal cellWidth = gridRect.width() / session.columns;
    const qreal cellHeight = gridRect.height() / session.rows;
    if (!positiveFinite(cellWidth) || !positiveFinite(cellHeight)) {
        return {};
    }
    return {
        clampedCellIndex((surfacePosition.x() - gridRect.left()) / cellWidth,
                         session.columns),
        clampedCellIndex((surfacePosition.y() - gridRect.top()) / cellHeight,
                         session.rows),
    };
}

std::optional<QPoint>
TerminalViewportLayout::strictCellAt(QPointF surfacePosition) const noexcept
{
    if (!std::isfinite(surfacePosition.x())
        || !std::isfinite(surfacePosition.y())
        || surfacePosition.x() < gridRect.left()
        || surfacePosition.x() >= gridRect.right()
        || surfacePosition.y() < gridRect.top()
        || surfacePosition.y() >= gridRect.bottom()) {
        return std::nullopt;
    }
    return clampedCellAt(surfacePosition);
}

std::optional<TerminalViewportLayout>
terminalViewportLayout(const TerminalViewportSpec &spec) noexcept
{
    if (!positiveFinite(spec.surfaceSize.width())
        || !positiveFinite(spec.surfaceSize.height())
        || !positiveFinite(spec.cellSize.width())
        || !positiveFinite(spec.cellSize.height())
        || !positiveFinite(spec.devicePixelRatio)) {
        return std::nullopt;
    }

    TerminalSessionGeometry geometry{
        .cellWidthPixels =
            boundedDevicePixels(spec.cellSize.width(), spec.devicePixelRatio),
        .cellHeightPixels =
            boundedDevicePixels(spec.cellSize.height(), spec.devicePixelRatio),
        .surfaceWidthPixels = boundedDevicePixels(spec.surfaceSize.width(),
                                                  spec.devicePixelRatio),
        .surfaceHeightPixels = boundedDevicePixels(spec.surfaceSize.height(),
                                                   spec.devicePixelRatio),
        .padding = {},
    };
    const TerminalSessionGeometry::Padding explicitPixels =
        explicitPadding(spec);
    geometry.columns = boundedCellCount(
        saturatingSubtract(geometry.surfaceWidthPixels, explicitPixels.left,
                           explicitPixels.right),
        geometry.cellWidthPixels);
    geometry.rows = boundedCellCount(
        saturatingSubtract(geometry.surfaceHeightPixels, explicitPixels.top,
                           explicitPixels.bottom),
        geometry.cellHeightPixels);
    geometry.padding =
        balancedPadding(geometry, explicitPixels, spec.padding.balance);

    const qreal left =
        logicalPixels(geometry.padding.left, spec.devicePixelRatio);
    const qreal top =
        logicalPixels(geometry.padding.top, spec.devicePixelRatio);
    const qreal cellWidth =
        logicalPixels(geometry.cellWidthPixels, spec.devicePixelRatio);
    const qreal cellHeight =
        logicalPixels(geometry.cellHeightPixels, spec.devicePixelRatio);
    return TerminalViewportLayout{
        .session = geometry,
        .gridRect = QRectF(left, top, geometry.columns * cellWidth,
                           geometry.rows * cellHeight),
    };
}

std::optional<TerminalSessionGeometry> terminalSessionGeometryForViewport(
    qreal width, qreal height, qreal cellWidth, qreal cellHeight,
    qreal devicePixelRatio, const TerminalPaddingOptions &padding) noexcept
{
    auto layout = terminalViewportLayout({
        .surfaceSize = QSizeF(width, height),
        .cellSize = QSizeF(cellWidth, cellHeight),
        .devicePixelRatio = devicePixelRatio,
        .padding = padding,
    });
    if (!layout) return std::nullopt;
    return layout->session;
}
