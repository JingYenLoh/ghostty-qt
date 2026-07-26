#include "terminal_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

bool positiveFinite(qreal value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

int boundedCellCount(qreal extent, qreal cellExtent) noexcept
{
    const long double cells =
        std::floor(static_cast<long double>(extent)
                   / static_cast<long double>(cellExtent));
    constexpr int maximum =
        static_cast<int>(std::numeric_limits<quint16>::max());
    if (!std::isfinite(cells) || cells >= static_cast<long double>(maximum)) {
        return maximum;
    }
    return std::max(1, static_cast<int>(cells));
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

} // namespace

std::optional<TerminalSessionGeometry>
terminalSessionGeometryForViewport(qreal width, qreal height, qreal cellWidth,
                                   qreal cellHeight,
                                   qreal devicePixelRatio) noexcept
{
    if (!positiveFinite(width) || !positiveFinite(height)
        || !positiveFinite(cellWidth) || !positiveFinite(cellHeight)
        || !positiveFinite(devicePixelRatio)) {
        return std::nullopt;
    }

    return TerminalSessionGeometry{
        .columns = boundedCellCount(width, cellWidth),
        .rows = boundedCellCount(height, cellHeight),
        .cellWidthPixels = boundedDevicePixels(cellWidth, devicePixelRatio),
        .cellHeightPixels = boundedDevicePixels(cellHeight, devicePixelRatio),
        .surfaceWidthPixels = boundedDevicePixels(width, devicePixelRatio),
        .surfaceHeightPixels = boundedDevicePixels(height, devicePixelRatio),
    };
}
