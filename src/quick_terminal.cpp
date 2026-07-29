#include "quick_terminal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace {

int nonnegativeDimension(int value) noexcept
{
    return std::max(0, value);
}

int saturatedDimension(float value) noexcept
{
    if (std::isnan(value) || value <= 0.0F) return 0;

    constexpr int maximum = std::numeric_limits<int>::max();
    if (!std::isfinite(value) || value >= static_cast<float>(maximum)) {
        return maximum;
    }
    return static_cast<int>(value);
}

int saturatedDimension(quint32 value) noexcept
{
    constexpr auto maximum =
        static_cast<quint32>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(value, maximum));
}

int saturatedCoordinate(qint64 value) noexcept
{
    return static_cast<int>(
        std::clamp<qint64>(value, std::numeric_limits<int>::min(),
                           std::numeric_limits<int>::max()));
}

QSize nonnegativeSize(QSize value) noexcept
{
    return {nonnegativeDimension(value.width()),
            nonnegativeDimension(value.height())};
}

int centeredCoordinate(int outputOrigin, int outputExtent,
                       int requestedExtent) noexcept
{
    return saturatedCoordinate(
        static_cast<qint64>(outputOrigin)
        + (static_cast<qint64>(outputExtent) - requestedExtent) / 2);
}

int trailingCoordinate(int outputOrigin, int outputExtent,
                       int requestedExtent) noexcept
{
    return saturatedCoordinate(static_cast<qint64>(outputOrigin) + outputExtent
                               - requestedExtent);
}

} // namespace

int quickTerminalExtentPixels(const QuickTerminalExtent &extent,
                              int parentDimension) noexcept
{
    const int parent = nonnegativeDimension(parentDimension);
    return std::visit(
        [parent](const auto &value) noexcept {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, QuickTerminalPercentage>) {
                // Keep both operations in f32 and in the same order as the
                // pinned Zig implementation: value / 100.0 * dimension.
                const float dimension = static_cast<float>(parent);
                const float pixels = value.value / 100.0F * dimension;
                return saturatedDimension(pixels);
            } else {
                static_assert(std::is_same_v<Value, QuickTerminalPixels>);
                return saturatedDimension(value.value);
            }
        },
        extent);
}

QSize quickTerminalSize(const QuickTerminalSize &size,
                        QuickTerminalPosition position,
                        QSize fullOutputSize) noexcept
{
    const QSize output = nonnegativeSize(fullOutputSize);
    const auto extentOr = [](const std::optional<QuickTerminalExtent> &extent,
                             int parent, int fallback) noexcept {
        return extent ? quickTerminalExtentPixels(*extent, parent) : fallback;
    };

    switch (position) {
    case QuickTerminalPosition::Left:
    case QuickTerminalPosition::Right:
        return {
            extentOr(size.primary, output.width(), 400),
            extentOr(size.secondary, output.height(), output.height()),
        };

    case QuickTerminalPosition::Top:
    case QuickTerminalPosition::Bottom:
        return {
            extentOr(size.secondary, output.width(), output.width()),
            extentOr(size.primary, output.height(), 400),
        };

    case QuickTerminalPosition::Center:
        if (output.width() >= output.height()) {
            return {
                extentOr(size.primary, output.width(), 800),
                extentOr(size.secondary, output.height(), 400),
            };
        }
        return {
            extentOr(size.secondary, output.width(), 400),
            extentOr(size.primary, output.height(), 800),
        };
    }

    return {};
}

QuickTerminalPlacementIntent
quickTerminalPlacementIntent(QuickTerminalPosition position,
                             int unanchoredMargin) noexcept
{
    const int margin = nonnegativeDimension(unanchoredMargin);
    QuickTerminalPlacementIntent intent{
        .anchors = {},
        .margins = QMargins(margin, margin, margin, margin),
    };

    const auto anchor = [&intent](Qt::Edge edge) {
        intent.anchors.setFlag(edge);
        switch (edge) {
        case Qt::LeftEdge: intent.margins.setLeft(0); break;
        case Qt::RightEdge: intent.margins.setRight(0); break;
        case Qt::TopEdge: intent.margins.setTop(0); break;
        case Qt::BottomEdge: intent.margins.setBottom(0); break;
        }
    };

    switch (position) {
    case QuickTerminalPosition::Top: anchor(Qt::TopEdge); break;
    case QuickTerminalPosition::Bottom: anchor(Qt::BottomEdge); break;
    case QuickTerminalPosition::Left: anchor(Qt::LeftEdge); break;
    case QuickTerminalPosition::Right: anchor(Qt::RightEdge); break;
    case QuickTerminalPosition::Center: break;
    }
    return intent;
}

QRect quickTerminalGeometry(QuickTerminalPosition position,
                            QRect outputGeometry, QSize requestedSize) noexcept
{
    const QSize outputSize = nonnegativeSize(outputGeometry.size());
    const QSize size = nonnegativeSize(requestedSize);
    const int centeredX = centeredCoordinate(outputGeometry.x(),
                                             outputSize.width(), size.width());
    const int centeredY = centeredCoordinate(
        outputGeometry.y(), outputSize.height(), size.height());

    switch (position) {
    case QuickTerminalPosition::Top:
        return {{centeredX, outputGeometry.y()}, size};
    case QuickTerminalPosition::Bottom:
        return {{centeredX,
                 trailingCoordinate(outputGeometry.y(), outputSize.height(),
                                    size.height())},
                size};
    case QuickTerminalPosition::Left:
        return {{outputGeometry.x(), centeredY}, size};
    case QuickTerminalPosition::Right:
        return {{trailingCoordinate(outputGeometry.x(), outputSize.width(),
                                    size.width()),
                 centeredY},
                size};
    case QuickTerminalPosition::Center: return {{centeredX, centeredY}, size};
    }

    return {};
}

QRect quickTerminalGeometry(const QuickTerminalOptions &options,
                            QRect outputGeometry) noexcept
{
    return quickTerminalGeometry(options.position, outputGeometry,
                                 quickTerminalSize(options.size,
                                                   options.position,
                                                   outputGeometry.size()));
}
