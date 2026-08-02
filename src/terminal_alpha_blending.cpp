#include "terminal_alpha_blending.h"

#include "terminal_custom_shader_compiler.h"

#include <QFile>
#include <QResource>
#include <algorithm>
#include <cmath>

static void initializeAlphaEncodeShaderResource()
{
    static const bool initialized = [] {
        Q_INIT_RESOURCE(ghostty_qt_terminal_custom_shader_shaders);
        return true;
    }();
    Q_UNUSED(initialized);
}

float terminalSrgbToLinear(float component) noexcept
{
    component = std::clamp(component, 0.0F, 1.0F);
    return component <= 0.04045F
        ? component / 12.92F
        : std::pow((component + 0.055F) / 1.055F, 2.4F);
}

QColor terminalLinearizedColor(const QColor &color)
{
    if (!color.isValid()) return color;
    const QColor rgb = color.toRgb();
    return QColor::fromRgbF(
        terminalSrgbToLinear(static_cast<float>(rgb.redF())),
        terminalSrgbToLinear(static_cast<float>(rgb.greenF())),
        terminalSrgbToLinear(static_cast<float>(rgb.blueF())), rgb.alphaF());
}

QColor terminalRenderingColor(const QColor &color,
                              TerminalAlphaBlending mode)
{
    return terminalUsesLinearBlending(mode) ? terminalLinearizedColor(color)
                                            : color;
}

const TerminalCustomShaderStage &terminalAlphaEncodeShaderStage()
{
    static const TerminalCustomShaderStage stage = [] {
        initializeAlphaEncodeShaderResource();
        const QString path = QStringLiteral(
            ":/ghostty-qt/shaders/terminal_alpha_encode.frag.qsb");
        QFile file(path);
        return TerminalCustomShaderStage{
            .sourcePath = QStringLiteral("builtin:alpha-blending"),
            .qsbPath = path,
            .cacheKey = QByteArrayLiteral("ghostty-qt-alpha-encode-v1"),
            .serializedShader = file.open(QIODevice::ReadOnly) ? file.readAll()
                                                                : QByteArray{},
        };
    }();
    return stage;
}
