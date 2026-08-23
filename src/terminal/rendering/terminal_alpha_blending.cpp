#include "terminal/rendering/terminal_alpha_blending.h"

#include "terminal/rendering/terminal_custom_shader_compiler.h"

#include <QFile>
#include <QImage>
#include <QResource>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

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

const TerminalSrgb8ToLinearLookup &terminalSrgb8ToLinearLookup() noexcept
{
    static const TerminalSrgb8ToLinearLookup lookup = [] {
        TerminalSrgb8ToLinearLookup result{};
        for (std::size_t component = 0; component < result.size();
             ++component) {
            result[component] =
                terminalSrgbToLinear(static_cast<float>(component) / 255.0F);
        }
        return result;
    }();
    return lookup;
}

namespace {

using TerminalPremultipliedSrgb8Lookup =
    std::array<quint8,
               terminalSrgb8ComponentCount * terminalSrgb8ComponentCount>;

constexpr std::size_t premultipliedLookupIndex(std::size_t component,
                                               std::size_t alpha) noexcept
{
    return alpha * terminalSrgb8ComponentCount + component;
}

const TerminalPremultipliedSrgb8Lookup &
terminalPremultipliedSrgb8Lookup() noexcept
{
    static const TerminalPremultipliedSrgb8Lookup lookup = [] {
        TerminalPremultipliedSrgb8Lookup result{};
        for (std::size_t alphaByte = 1; alphaByte < terminalSrgb8ComponentCount;
             ++alphaByte) {
            const float alpha = static_cast<float>(alphaByte) / 255.0F;
            for (std::size_t component = 0;
                 component < terminalSrgb8ComponentCount; ++component) {
                const float straight =
                    std::clamp(static_cast<float>(component)
                                   / static_cast<float>(alphaByte),
                               0.0F, 1.0F);
                const float linearPremultiplied =
                    terminalSrgbToLinear(straight) * alpha;
                const int byte = std::clamp(
                    static_cast<int>(std::lround(linearPremultiplied * 255.0F)),
                    0, 255);
                result[premultipliedLookupIndex(component, alphaByte)] =
                    static_cast<quint8>(byte);
            }
        }
        return result;
    }();
    return lookup;
}

} // namespace

void terminalLinearizePremultipliedSrgb8(QImage &image)
{
    if (image.isNull()) return;
    Q_ASSERT(image.format() == QImage::Format_ARGB32_Premultiplied);
    if (image.format() != QImage::Format_ARGB32_Premultiplied) return;

    const TerminalPremultipliedSrgb8Lookup &lookup =
        terminalPremultipliedSrgb8Lookup();
    for (int y = 0; y < image.height(); ++y) {
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const std::size_t alpha =
                static_cast<std::size_t>(qAlpha(pixels[x]));
            if (alpha == 0) {
                pixels[x] = 0;
                continue;
            }
            const auto channel = [&lookup, alpha](int component) {
                return lookup[premultipliedLookupIndex(
                    static_cast<std::size_t>(component), alpha)];
            };
            pixels[x] =
                qRgba(channel(qRed(pixels[x])), channel(qGreen(pixels[x])),
                      channel(qBlue(pixels[x])), static_cast<int>(alpha));
        }
    }
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

QColor terminalRenderingColor(const QColor &color, TerminalAlphaBlending mode)
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
            .serializedShader =
                file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{},
        };
    }();
    return stage;
}
