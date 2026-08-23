#pragma once

#include <QImage>
#include <QSize>
#include <QtGlobal>

#include <cstdint>
#include <optional>
#include <span>

enum class TerminalKittyPixelFormat : quint8 {
    Rgb,
    Rgba,
    GrayAlpha,
    Gray,
};

struct TerminalKittyImageMaterialization final {
    bool fullyOpaque = false;
    QImage straightRgba;
};

// Copy a tightly packed decoded Kitty image into worker-owned, straight-alpha
// RGBA8 storage. The source span must contain exactly size.width() *
// size.height() pixels in the selected format.
[[nodiscard]] std::optional<TerminalKittyImageMaterialization>
terminalMaterializeKittyImage(QSize size, TerminalKittyPixelFormat format,
                              std::span<const std::uint8_t> pixels);
