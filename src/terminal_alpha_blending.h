#pragma once

#include <QColor>
#include <QMetaType>

struct TerminalCustomShaderStage;

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
[[nodiscard]] QColor terminalLinearizedColor(const QColor &color);
[[nodiscard]] QColor terminalRenderingColor(const QColor &color,
                                            TerminalAlphaBlending mode);
[[nodiscard]] const TerminalCustomShaderStage &
terminalAlphaEncodeShaderStage();

Q_DECLARE_METATYPE(TerminalAlphaBlending)
