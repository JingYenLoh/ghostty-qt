#include "terminal_alpha_blending.h"
#include "terminal_custom_shader_compiler.h"
#include "terminal_custom_shader_qsg.h"

#include <QTest>
#include <rhi/qshader.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

int referenceLinearPremultipliedByte(int component, int alphaByte)
{
    if (alphaByte == 0) return 0;
    const float alpha = static_cast<float>(alphaByte) / 255.0F;
    const float straight = std::clamp(static_cast<float>(component)
                                          / static_cast<float>(alphaByte),
                                      0.0F, 1.0F);
    return std::clamp(static_cast<int>(std::lround(
                          terminalSrgbToLinear(straight) * alpha * 255.0F)),
                      0, 255);
}

} // namespace

class TerminalAlphaBlendingTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void convertsSrgbComponentsToLinear();
    void cachesEverySrgb8ComponentExactly();
    void preservesEveryPremultipliedByteConversionExactly();
    void convertsRenderingColorsWithoutChangingAlpha();
    void embedsFinalEncodeShaderWithSharedAbi();
};

void TerminalAlphaBlendingTest::convertsSrgbComponentsToLinear()
{
    QCOMPARE(terminalSrgbToLinear(0.0F), 0.0F);
    QCOMPARE(terminalSrgbToLinear(1.0F), 1.0F);
    QVERIFY(std::abs(terminalSrgbToLinear(0.5F) - 0.21404114F) < 0.000001F);
}

void TerminalAlphaBlendingTest::cachesEverySrgb8ComponentExactly()
{
    const TerminalSrgb8ToLinearLookup &lookup = terminalSrgb8ToLinearLookup();
    for (std::size_t component = 0; component < lookup.size(); ++component) {
        const float expected =
            terminalSrgbToLinear(static_cast<float>(component) / 255.0F);
        QCOMPARE(lookup[component], expected);
    }
}

void TerminalAlphaBlendingTest::
    preservesEveryPremultipliedByteConversionExactly()
{
    constexpr int componentCount =
        static_cast<int>(terminalSrgb8ComponentCount);
    QImage image(componentCount, componentCount,
                 QImage::Format_ARGB32_Premultiplied);
    QVERIFY(!image.isNull());
    for (int alphaByte = 0; alphaByte < componentCount; ++alphaByte) {
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(alphaByte));
        for (int component = 0; component < componentCount; ++component) {
            pixels[component] =
                qRgba(component, component, component, alphaByte);
        }
    }

    terminalLinearizePremultipliedSrgb8(image);

    for (int alphaByte = 0; alphaByte < componentCount; ++alphaByte) {
        const auto *pixels =
            reinterpret_cast<const QRgb *>(image.constScanLine(alphaByte));
        for (int component = 0; component < componentCount; ++component) {
            const QRgb actual = pixels[component];
            const int expected =
                referenceLinearPremultipliedByte(component, alphaByte);
            if (qAlpha(actual) != alphaByte || qRed(actual) != expected
                || qGreen(actual) != expected || qBlue(actual) != expected) {
                const QString diagnostic =
                    QStringLiteral(
                        "component %1, alpha %2: expected rgba(%3, %3, %3, %2), "
                        "got rgba(%4, %5, %6, %7)")
                        .arg(component)
                        .arg(alphaByte)
                        .arg(expected)
                        .arg(qRed(actual))
                        .arg(qGreen(actual))
                        .arg(qBlue(actual))
                        .arg(qAlpha(actual));
                QFAIL(qPrintable(diagnostic));
            }
        }
    }
}

void TerminalAlphaBlendingTest::
    convertsRenderingColorsWithoutChangingAlpha()
{
    const QColor source = QColor::fromRgbF(0.5, 0.25, 0.75, 0.375);
    QCOMPARE(terminalRenderingColor(source, TerminalAlphaBlending::Native),
             source);
    const QColor linear =
        terminalRenderingColor(source, TerminalAlphaBlending::Linear);
    QVERIFY(std::abs(linear.redF() - terminalSrgbToLinear(0.5F)) < 0.0001F);
    QVERIFY(std::abs(linear.greenF() - terminalSrgbToLinear(0.25F))
            < 0.0001F);
    QVERIFY(std::abs(linear.blueF() - terminalSrgbToLinear(0.75F)) < 0.0001F);
    QVERIFY(std::abs(linear.alphaF() - source.alphaF()) < 0.0001F);
    QCOMPARE(terminalRenderingColor(
                 source, TerminalAlphaBlending::LinearCorrected),
             linear);
}

void TerminalAlphaBlendingTest::embedsFinalEncodeShaderWithSharedAbi()
{
    const TerminalCustomShaderStage &stage = terminalAlphaEncodeShaderStage();
    QVERIFY(!stage.qsbPath.isEmpty());
    QVERIFY(!stage.serializedShader.isEmpty());
    const QShader shader = QShader::fromSerialized(stage.serializedShader);
    QVERIFY(shader.isValid());
    QCOMPARE(shader.stage(), QShader::FragmentStage);
    const auto blocks = shader.description().uniformBlocks();
    QCOMPARE(blocks.size(), 1);
    QCOMPARE(blocks.constFirst().binding, 0);
    QCOMPARE(blocks.constFirst().size,
             static_cast<int>(TerminalCustomShaderUniformLayout::size));
    const auto samplers = shader.description().combinedImageSamplers();
    QCOMPARE(samplers.size(), 1);
    QCOMPARE(samplers.constFirst().name, QByteArray("iChannel0"));
    QCOMPARE(samplers.constFirst().binding, 1);
}

QTEST_APPLESS_MAIN(TerminalAlphaBlendingTest)

#include "test_terminal_alpha_blending.moc"
