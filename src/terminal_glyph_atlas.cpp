#include "terminal_glyph_atlas.h"

#include <QTransform>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace {

struct RasterizedGlyph {
    TerminalGlyphAtlasKey key;
    QImage alphaMap;
    QRect opaqueRect;
    QPointF logicalBearing;
    QSizeF logicalSize;

    [[nodiscard]] bool blank() const noexcept { return opaqueRect.isEmpty(); }
};

struct ShelfPacking {
    QSize size;
    QVector<QRect> paddedRects;
};

[[nodiscard]] QRect opaqueBounds(const QImage &image) noexcept
{
    if (image.isNull() || image.format() != QImage::Format_Alpha8) return {};

    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const uchar *const row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            if (row[x] == 0) continue;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }
    return right >= left && bottom >= top
        ? QRect(QPoint(left, top), QPoint(right, bottom))
        : QRect{};
}

[[nodiscard]] QImage alphaCoverageImage(QImage image)
{
    if (image.isNull() || image.format() == QImage::Format_Alpha8) {
        return image;
    }
    // Qt 6.8 documents PixelAntialiasing as Indexed8, while newer FreeType
    // backends return Alpha8 directly. In both legacy one-byte formats each
    // stored byte is coverage; a generic color conversion can instead turn
    // an opaque grayscale palette into all-255 alpha.
    if (image.format() != QImage::Format_Indexed8
        && image.format() != QImage::Format_Grayscale8) {
        // Color-font rasters need their own RGBA path and stay on QSGTextNode.
        return {};
    }
    QImage alpha(image.size(), QImage::Format_Alpha8);
    if (alpha.isNull()) return {};
    for (int row = 0; row < image.height(); ++row) {
        std::memcpy(alpha.scanLine(row), image.constScanLine(row),
                    static_cast<std::size_t>(image.width()));
    }
    return alpha;
}

[[nodiscard]] std::optional<ShelfPacking>
packShelves(const QVector<RasterizedGlyph> &glyphs, int width, int padding,
            int maximumDimension)
{
    ShelfPacking result;
    result.paddedRects.resize(glyphs.size());

    int x = 0;
    int y = 0;
    int shelfHeight = 0;
    int usedWidth = 0;
    for (qsizetype index = 0; index < glyphs.size(); ++index) {
        const RasterizedGlyph &glyph = glyphs.at(index);
        if (glyph.blank()) continue;

        const int contentWidth = glyph.opaqueRect.width();
        const int contentHeight = glyph.opaqueRect.height();
        if (contentWidth > maximumDimension - padding * 2
            || contentHeight > maximumDimension - padding * 2) {
            return std::nullopt;
        }
        const int paddedWidth = contentWidth + padding * 2;
        const int paddedHeight = contentHeight + padding * 2;
        if (paddedWidth > width) return std::nullopt;

        if (x > 0 && paddedWidth > width - x) {
            y += shelfHeight;
            x = 0;
            shelfHeight = 0;
        }
        if (paddedHeight > maximumDimension - y) return std::nullopt;

        result.paddedRects[index] = QRect(x, y, paddedWidth, paddedHeight);
        x += paddedWidth;
        usedWidth = std::max(usedWidth, x);
        shelfHeight = std::max(shelfHeight, paddedHeight);
    }

    result.size = QSize(std::max(usedWidth, 1), std::max(y + shelfHeight, 1));
    return result;
}

[[nodiscard]] std::optional<ShelfPacking>
packGlyphs(const QVector<RasterizedGlyph> &glyphs, int padding,
           int maximumDimension)
{
    qint64 area = 0;
    int widest = 1;
    bool hasPixels = false;
    for (const RasterizedGlyph &glyph : glyphs) {
        if (glyph.blank()) continue;
        hasPixels = true;
        if (glyph.opaqueRect.width() > maximumDimension - padding * 2
            || glyph.opaqueRect.height() > maximumDimension - padding * 2) {
            return std::nullopt;
        }
        const int width = glyph.opaqueRect.width() + padding * 2;
        const int height = glyph.opaqueRect.height() + padding * 2;
        widest = std::max(widest, width);
        const qint64 glyphArea = static_cast<qint64>(width) * height;
        if (area > std::numeric_limits<qint64>::max() - glyphArea) {
            return std::nullopt;
        }
        area += glyphArea;
    }
    if (!hasPixels) {
        return ShelfPacking{.size = QSize(1, 1), .paddedRects = {}};
    }

    const qint64 maximumArea =
        static_cast<qint64>(maximumDimension) * maximumDimension;
    if (area > maximumArea) return std::nullopt;

    const auto root =
        static_cast<int>(std::ceil(std::sqrt(static_cast<long double>(area))));
    int width = std::clamp(root, widest, maximumDimension);
    while (true) {
        if (std::optional<ShelfPacking> packing =
                packShelves(glyphs, width, padding, maximumDimension)) {
            return packing;
        }
        if (width == maximumDimension) return std::nullopt;
        width = width > maximumDimension / 2 ? maximumDimension : width * 2;
    }
}

[[nodiscard]] std::optional<RasterizedGlyph>
rasterizeGlyph(const TerminalGlyphAtlasKey &key, qreal devicePixelRatio)
{
    if (!key.font.isValid()) {
        return std::nullopt;
    }

    const QTransform transform =
        QTransform::fromScale(devicePixelRatio, devicePixelRatio);
    const QRectF bounds = key.font.boundingRect(key.glyphIndex);
    QImage alphaMap = key.font.alphaMapForGlyph(
        key.glyphIndex, QRawFont::PixelAntialiasing, transform);
    if (alphaMap.isNull() && !bounds.isEmpty()) return std::nullopt;
    if (!alphaMap.isNull() && alphaMap.format() != QImage::Format_Alpha8) {
        alphaMap = alphaCoverageImage(std::move(alphaMap));
        if (alphaMap.isNull()) return std::nullopt;
    }

    const QRect opaqueRect = opaqueBounds(alphaMap);
    if (opaqueRect.isEmpty()) {
        return RasterizedGlyph{
            .key = key,
            .alphaMap = std::move(alphaMap),
            .opaqueRect = {},
            .logicalBearing = {},
            .logicalSize = {},
        };
    }

    const qreal physicalLeft = std::floor(bounds.left() * devicePixelRatio);
    const qreal physicalTop = std::floor(bounds.top() * devicePixelRatio);
    return RasterizedGlyph{
        .key = key,
        .alphaMap = std::move(alphaMap),
        .opaqueRect = opaqueRect,
        .logicalBearing =
            QPointF((physicalLeft + opaqueRect.left()) / devicePixelRatio,
                    (physicalTop + opaqueRect.top()) / devicePixelRatio),
        .logicalSize = QSizeF(opaqueRect.width() / devicePixelRatio,
                              opaqueRect.height() / devicePixelRatio),
    };
}

void copyGlyphMask(QImage &atlas, const RasterizedGlyph &glyph,
                   const QRect &destination)
{
    for (int row = 0; row < glyph.opaqueRect.height(); ++row) {
        uchar *const target =
            atlas.scanLine(destination.y() + row) + destination.x();
        const uchar *const source =
            glyph.alphaMap.constScanLine(glyph.opaqueRect.y() + row)
            + glyph.opaqueRect.x();
        std::memcpy(target, source,
                    static_cast<size_t>(glyph.opaqueRect.width()));
    }
}

} // namespace

size_t qHash(const TerminalGlyphAtlasKey &key, size_t seed) noexcept
{
    return ::qHash(key.glyphIndex, ::qHash(key.font, seed));
}

QRectF TerminalGlyphAtlasEntry::logicalDestination(
    const QPointF &baselinePosition) const noexcept
{
    return QRectF(baselinePosition + logicalBearing, logicalSize);
}

std::optional<TerminalGlyphAtlas>
TerminalGlyphAtlas::build(std::span<const TerminalGlyphAtlasKey> glyphs,
                          TerminalGlyphAtlasOptions options)
{
    if (!std::isfinite(options.devicePixelRatio)
        || options.devicePixelRatio <= 0.0 || options.paddingPixels < 0
        || options.maxTextureDimension <= 0
        || options.paddingPixels > options.maxTextureDimension / 2) {
        return std::nullopt;
    }

    QVector<RasterizedGlyph> rasterized;
    rasterized.reserve(static_cast<qsizetype>(glyphs.size()));
    QHash<TerminalGlyphAtlasKey, bool> seen;
    seen.reserve(static_cast<qsizetype>(glyphs.size()));
    for (const TerminalGlyphAtlasKey &key : glyphs) {
        if (seen.contains(key)) continue;
        seen.insert(key, true);
        std::optional<RasterizedGlyph> glyph =
            rasterizeGlyph(key, options.devicePixelRatio);
        if (!glyph) return std::nullopt;
        rasterized.append(std::move(*glyph));
    }

    std::optional<ShelfPacking> packing = packGlyphs(
        rasterized, options.paddingPixels, options.maxTextureDimension);
    if (!packing) return std::nullopt;

    QImage image(packing->size, QImage::Format_Alpha8);
    if (image.isNull()) return std::nullopt;
    image.fill(0);

    QHash<TerminalGlyphAtlasKey, TerminalGlyphAtlasEntry> entries;
    entries.reserve(rasterized.size());
    for (qsizetype index = 0; index < rasterized.size(); ++index) {
        RasterizedGlyph &glyph = rasterized[index];
        TerminalGlyphAtlasEntry entry;
        if (!glyph.blank()) {
            entry.blank = false;
            entry.paddedPixelRect = packing->paddedRects.at(index);
            entry.pixelRect = entry.paddedPixelRect.adjusted(
                options.paddingPixels, options.paddingPixels,
                -options.paddingPixels, -options.paddingPixels);
            entry.normalizedTextureRect = QRectF(
                static_cast<qreal>(entry.pixelRect.x()) / image.width(),
                static_cast<qreal>(entry.pixelRect.y()) / image.height(),
                static_cast<qreal>(entry.pixelRect.width()) / image.width(),
                static_cast<qreal>(entry.pixelRect.height()) / image.height());
            entry.logicalBearing = glyph.logicalBearing;
            entry.logicalSize = glyph.logicalSize;
            copyGlyphMask(image, glyph, entry.pixelRect);
        }
        entries.insert(std::move(glyph.key), std::move(entry));
    }

    return TerminalGlyphAtlas(std::move(image), std::move(entries),
                              options.devicePixelRatio, options.paddingPixels);
}

TerminalGlyphAtlas::TerminalGlyphAtlas(
    QImage image, QHash<TerminalGlyphAtlasKey, TerminalGlyphAtlasEntry> entries,
    qreal devicePixelRatio, int paddingPixels)
    : image_(std::move(image))
    , entries_(std::move(entries))
    , devicePixelRatio_(devicePixelRatio)
    , paddingPixels_(paddingPixels)
{}

const TerminalGlyphAtlasEntry *
TerminalGlyphAtlas::lookup(const TerminalGlyphAtlasKey &key) const noexcept
{
    const auto iterator = entries_.constFind(key);
    return iterator != entries_.cend() ? std::addressof(iterator.value())
                                       : nullptr;
}

const TerminalGlyphAtlasEntry *
TerminalGlyphAtlas::lookup(const QRawFont &font,
                           quint32 glyphIndex) const noexcept
{
    return lookup({.font = font, .glyphIndex = glyphIndex});
}

const QImage &TerminalGlyphAtlas::image() const noexcept
{
    return image_;
}

QImage TerminalGlyphAtlas::textureImage() const
{
    QImage texture(image_.size(), QImage::Format_RGBA8888_Premultiplied);
    if (texture.isNull()) return {};
    for (int y = 0; y < image_.height(); ++y) {
        const uchar *const source = image_.constScanLine(y);
        uchar *const destination = texture.scanLine(y);
        for (int x = 0; x < image_.width(); ++x) {
            const uchar coverage = source[x];
            destination[x * 4] = coverage;
            destination[x * 4 + 1] = coverage;
            destination[x * 4 + 2] = coverage;
            destination[x * 4 + 3] = coverage;
        }
    }
    return texture;
}

QSize TerminalGlyphAtlas::pixelSize() const noexcept
{
    return image_.size();
}

qsizetype TerminalGlyphAtlas::byteSize() const noexcept
{
    return image_.sizeInBytes();
}

qsizetype TerminalGlyphAtlas::textureByteSize() const noexcept
{
    constexpr qsizetype bytesPerPixel = 4;
    return static_cast<qsizetype>(image_.width()) * image_.height()
        * bytesPerPixel;
}

qsizetype TerminalGlyphAtlas::entryCount() const noexcept
{
    return entries_.size();
}

qreal TerminalGlyphAtlas::devicePixelRatio() const noexcept
{
    return devicePixelRatio_;
}

int TerminalGlyphAtlas::paddingPixels() const noexcept
{
    return paddingPixels_;
}
