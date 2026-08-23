#pragma once

#include <QColor>
#include <QMetaType>

#include <array>
#include <cstddef>

struct TerminalCustomShaderStage;
class QImage;

enum class TerminalAlphaBlending {
    Native,
    Linear,
    LinearCorrected,
};

[[nodiscard]] constexpr bool
terminalUsesLinearBlending(TerminalAlphaBlending mode) noexcept
{
    return mode != TerminalAlphaBlending::Native;
}

[[nodiscard]] float terminalSrgbToLinear(float component) noexcept;

inline constexpr std::size_t terminalSrgb8ComponentCount = 256;
using TerminalSrgb8ToLinearLookup =
    std::array<float, terminalSrgb8ComponentCount>;

// Exact transfer values for every 8-bit sRGB component. This avoids repeated
// power-function evaluation in cell rendering while leaving the float API
// available for QColor values with more than eight bits of precision.
[[nodiscard]] const TerminalSrgb8ToLinearLookup &
terminalSrgb8ToLinearLookup() noexcept;

// Convert an ARGB32 premultiplied image from sRGB to linear-premultiplied
// channels in place. Alpha is preserved exactly; fully transparent pixels are
// normalized to zero. Other image formats are rejected in debug builds and
// left unchanged in release builds.
void terminalLinearizePremultipliedSrgb8(QImage &image);

[[nodiscard]] QColor terminalLinearizedColor(const QColor &color);
[[nodiscard]] QColor terminalRenderingColor(const QColor &color,
                                            TerminalAlphaBlending mode);
[[nodiscard]] const TerminalCustomShaderStage &terminalAlphaEncodeShaderStage();

Q_DECLARE_METATYPE(TerminalAlphaBlending)
