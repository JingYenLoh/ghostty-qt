#pragma once

#include <QImage>
#include <QRectF>
#include <QSizeF>
#include <QVector>
#include <QtGlobal>

#include <memory>
#include <optional>
#include <utility>

enum class TerminalKittyGraphicsLayer : quint8 {
    BelowBackground,
    BelowText,
    AboveText,
};

// Immutable, worker-owned copy of one libghostty Kitty image. libghostty lends
// its pixel pointer only until the next terminal mutation, so no borrowed
// storage may cross the session-thread boundary. Translucent images retain
// straight RGB and alpha in separate opaque planes so Qt's texture upload
// cannot premultiply before linear filtering. Fully opaque images need only
// the RGB plane.
struct TerminalKittyGraphicsImage {
    quint32 imageId = 0;
    quint64 generation = 0;
    // Opaque images omit alphaPlane. Hardware rendering samples one
    // scene-wide white alpha texture and software rendering can upload the
    // straight RGB plane directly.
    bool fullyOpaque = false;
    QImage straightRgbPlane;
    QImage alphaPlane;
};

// One expanded, non-virtual Kitty placement. Geometry is captured in the
// physical-pixel coordinate system used by libghostty. The render thread
// projects it to Qt logical coordinates using the current DPR.
struct TerminalKittyGraphicsPlacement {
    std::shared_ptr<const TerminalKittyGraphicsImage> image;
    quint32 placementId = 0;
    qint32 z = 0;
    TerminalKittyGraphicsLayer layer = TerminalKittyGraphicsLayer::AboveText;
    qint32 viewportColumn = 0;
    qint32 viewportRow = 0;
    quint32 xOffsetPixels = 0;
    quint32 yOffsetPixels = 0;
    quint32 destinationWidthPixels = 0;
    quint32 destinationHeightPixels = 0;
    quint32 sourceX = 0;
    quint32 sourceY = 0;
    quint32 sourceWidth = 0;
    quint32 sourceHeight = 0;

    friend bool operator==(const TerminalKittyGraphicsPlacement &left,
                           const TerminalKittyGraphicsPlacement &right)
    {
        const auto imageIdentity = [](const auto &candidate) {
            return std::pair{
                candidate != nullptr ? candidate->imageId : quint32{},
                candidate != nullptr ? candidate->generation : quint64{},
            };
        };
        return imageIdentity(left.image) == imageIdentity(right.image)
            && left.placementId == right.placementId && left.z == right.z
            && left.layer == right.layer
            && left.viewportColumn == right.viewportColumn
            && left.viewportRow == right.viewportRow
            && left.xOffsetPixels == right.xOffsetPixels
            && left.yOffsetPixels == right.yOffsetPixels
            && left.destinationWidthPixels == right.destinationWidthPixels
            && left.destinationHeightPixels == right.destinationHeightPixels
            && left.sourceX == right.sourceX && left.sourceY == right.sourceY
            && left.sourceWidth == right.sourceWidth
            && left.sourceHeight == right.sourceHeight;
    }
};

struct TerminalKittyGraphicsSnapshot {
    quint64 storageGeneration = 0;
    quint32 cellWidthPixels = 1;
    quint32 cellHeightPixels = 1;
    QVector<TerminalKittyGraphicsPlacement> placements;
    // libghostty exposes virtual placement definitions but not their expanded
    // viewport fragments. Preserve their presence for diagnostics and future
    // public-API integration while rendering ordinary placements normally.
    bool containsVirtualPlacements = false;

    friend bool operator==(const TerminalKittyGraphicsSnapshot &,
                           const TerminalKittyGraphicsSnapshot &) = default;
};

struct TerminalKittyGraphicsRenderPlacement {
    std::shared_ptr<const TerminalKittyGraphicsImage> image;
    quint32 placementId = 0;
    qint32 z = 0;
    TerminalKittyGraphicsLayer layer = TerminalKittyGraphicsLayer::AboveText;
    QRectF destination;
    QRectF source;
};

// Resolve physical Kitty placement geometry into the grid's local logical
// coordinate space and crop both rectangles at the visible grid boundary.
[[nodiscard]] std::optional<TerminalKittyGraphicsRenderPlacement>
terminalKittyGraphicsRenderPlacement(
    const TerminalKittyGraphicsPlacement &placement, const QSizeF &cellSize,
    const QSizeF &terminalCellPixelSize, const QRectF &gridViewport) noexcept;
