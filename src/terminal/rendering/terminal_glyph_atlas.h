#pragma once

#include <QHash>
#include <QImage>
#include <QPointF>
#include <QRawFont>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <span>

struct TerminalGlyphAtlasKey {
    QRawFont font;
    quint32 glyphIndex = 0;

    bool operator==(const TerminalGlyphAtlasKey &) const = default;
};

[[nodiscard]] size_t qHash(const TerminalGlyphAtlasKey &key,
                           size_t seed = 0) noexcept;

struct TerminalGlyphAtlasEntry {
    // The allocation including transparent sampling padding, followed by the
    // actual glyph mask inside that allocation.
    QRect paddedPixelRect;
    QRect pixelRect;
    QRectF normalizedTextureRect;

    // Offset from the glyph-run baseline and mask size in Qt logical pixels.
    // Adding logicalBearing to a shaped glyph position produces the top-left
    // destination vertex for pixelRect.
    QPointF logicalBearing;
    QSizeF logicalSize;
    bool blank = true;

    [[nodiscard]] QRectF
    logicalDestination(const QPointF &baselinePosition) const noexcept;
};

struct TerminalGlyphAtlasOptions {
    qreal devicePixelRatio = 1.0;
    int paddingPixels = 1;
    int maxTextureDimension = 4'096;
};

// An immutable Alpha8 atlas assembled entirely through public Qt APIs. Build
// returns nullopt for invalid fonts/options, impossible packing, or image
// allocation/rasterization failures. Shelves follow first-occurrence input
// order, making their padded rectangles stable for a stable request list.
// Blank glyphs are successful cached entries and consume no shelf space.
class TerminalGlyphAtlas final {
public:
    [[nodiscard]] static std::optional<TerminalGlyphAtlas>
    build(std::span<const TerminalGlyphAtlasKey> glyphs,
          TerminalGlyphAtlasOptions options = {});

    [[nodiscard]] const TerminalGlyphAtlasEntry *
    lookup(const TerminalGlyphAtlasKey &key) const noexcept;
    [[nodiscard]] const TerminalGlyphAtlasEntry *
    lookup(const QRawFont &font, quint32 glyphIndex) const noexcept;

    [[nodiscard]] const QImage &image() const noexcept;
    // Public QSG texture upload does not expose a portable swizzle for an
    // Alpha8 image to custom materials. Expand coverage into every channel so
    // OpenGL and Vulkan shaders observe the same value without private Qt API.
    [[nodiscard]] QImage textureImage() const;
    [[nodiscard]] QSize pixelSize() const noexcept;
    [[nodiscard]] qsizetype byteSize() const noexcept;
    [[nodiscard]] qsizetype textureByteSize() const noexcept;
    [[nodiscard]] qsizetype entryCount() const noexcept;
    [[nodiscard]] qreal devicePixelRatio() const noexcept;
    [[nodiscard]] int paddingPixels() const noexcept;

private:
    TerminalGlyphAtlas(
        QImage image,
        QHash<TerminalGlyphAtlasKey, TerminalGlyphAtlasEntry> entries,
        qreal devicePixelRatio, int paddingPixels);

    QImage image_;
    QHash<TerminalGlyphAtlasKey, TerminalGlyphAtlasEntry> entries_;
    qreal devicePixelRatio_ = 1.0;
    int paddingPixels_ = 1;
};
