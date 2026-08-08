#include "terminal_kitty_image_materialization.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

template <std::size_t bytesPerPixel, std::size_t alphaOffset>
bool allAlphaBytesOpaque(std::span<const std::uint8_t> pixels)
{
    static_assert(bytesPerPixel <= sizeof(std::uint64_t));
    static_assert(sizeof(std::uint64_t) % bytesPerPixel == 0);
    static_assert(alphaOffset < bytesPerPixel);

    constexpr std::uint64_t alphaMask = [] {
        std::array<std::uint8_t, sizeof(std::uint64_t)> bytes{};
        for (std::size_t pixel = 0;
             pixel < sizeof(std::uint64_t) / bytesPerPixel; ++pixel) {
            bytes[pixel * bytesPerPixel + alphaOffset] = 0xffU;
        }
        return std::bit_cast<std::uint64_t>(bytes);
    }();

    std::size_t offset = 0;
    while (pixels.size() - offset >= sizeof(std::uint64_t)) {
        std::uint64_t word = 0;
        std::memcpy(&word, pixels.data() + offset, sizeof(word));
        if ((word & alphaMask) != alphaMask) return false;
        offset += sizeof(word);
    }

    while (offset < pixels.size()) {
        if (pixels[offset + alphaOffset] != 0xffU) return false;
        offset += bytesPerPixel;
    }
    return true;
}

std::optional<QImage> convertPackedQtImage(QSize size,
                                           QImage::Format sourceFormat,
                                           std::span<const std::uint8_t> pixels,
                                           std::size_t sourceRowBytes)
{
    // The explicit bytesPerLine overload accepts Kitty's tightly packed odd
    // strides. Converting to a different format produces independent, owned
    // RGBA storage before the borrowed view leaves this scope.
    const QImage view(pixels.data(), size.width(), size.height(),
                      static_cast<qsizetype>(sourceRowBytes), sourceFormat);
    QImage converted = view.convertToFormat(QImage::Format_RGBA8888);

    if (converted.isNull()) return std::nullopt;
    converted.setDevicePixelRatio(1.0);
    return converted;
}

} // namespace

std::optional<TerminalKittyImageMaterialization>
terminalMaterializeKittyImage(QSize size, TerminalKittyPixelFormat format,
                              std::span<const std::uint8_t> pixels)
{
    if (size.width() <= 0 || size.height() <= 0) return std::nullopt;

    std::size_t bytesPerPixel = 0;
    switch (format) {
    case TerminalKittyPixelFormat::Rgb: bytesPerPixel = 3; break;
    case TerminalKittyPixelFormat::Rgba: bytesPerPixel = 4; break;
    case TerminalKittyPixelFormat::GrayAlpha: bytesPerPixel = 2; break;
    case TerminalKittyPixelFormat::Gray: bytesPerPixel = 1; break;
    default: return std::nullopt;
    }

    const auto width = static_cast<std::size_t>(size.width());
    const auto height = static_cast<std::size_t>(size.height());
    constexpr std::size_t maximumSize = std::numeric_limits<std::size_t>::max();
    if (width > maximumSize / bytesPerPixel) return std::nullopt;
    const std::size_t sourceRowBytes = width * bytesPerPixel;
    if (height > maximumSize / sourceRowBytes) return std::nullopt;
    const std::size_t expectedBytes = sourceRowBytes * height;
    if (pixels.size() != expectedBytes
        || sourceRowBytes
            > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        return std::nullopt;
    }

    if (format == TerminalKittyPixelFormat::Rgb
        || format == TerminalKittyPixelFormat::Gray) {
        const QImage::Format sourceFormat =
            format == TerminalKittyPixelFormat::Rgb ? QImage::Format_RGB888
                                                    : QImage::Format_Grayscale8;
        auto rgba =
            convertPackedQtImage(size, sourceFormat, pixels, sourceRowBytes);
        if (!rgba.has_value()) return std::nullopt;
        return TerminalKittyImageMaterialization{
            .fullyOpaque = true,
            .straightRgba = std::move(*rgba),
        };
    }

    QImage rgba(size, QImage::Format_RGBA8888);
    if (rgba.isNull()) return std::nullopt;
    rgba.setDevicePixelRatio(1.0);

    bool fullyOpaque = false;
    if (format == TerminalKittyPixelFormat::Rgba) {
        fullyOpaque = allAlphaBytesOpaque<4, 3>(pixels);
        // Every RGBA8888 row is already 32-bit aligned, so it has the same
        // width * 4 stride as Kitty's tightly packed payload.
        std::memcpy(rgba.bits(), pixels.data(), expectedBytes);
    } else {
        fullyOpaque = allAlphaBytesOpaque<2, 1>(pixels);
        std::uint8_t *destination = rgba.bits();
        const std::size_t pixelCount = width * height;
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const std::uint8_t gray = pixels[pixel * 2];
            destination[pixel * 4] = gray;
            destination[pixel * 4 + 1] = gray;
            destination[pixel * 4 + 2] = gray;
            destination[pixel * 4 + 3] = pixels[pixel * 2 + 1];
        }
    }

    return TerminalKittyImageMaterialization{
        .fullyOpaque = fullyOpaque,
        .straightRgba = std::move(rgba),
    };
}
