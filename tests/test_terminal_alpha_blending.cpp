#include "terminal_alpha_blending.h"
#include "terminal_custom_shader_compiler.h"
#include "terminal_custom_shader_qsg.h"

#include <QTest>
#include <rhi/qshader.h>

#include <cmath>

class TerminalAlphaBlendingTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void convertsSrgbComponentsToLinear();
    void convertsRenderingColorsWithoutChangingAlpha();
    void embedsFinalEncodeShaderWithSharedAbi();
};

void TerminalAlphaBlendingTest::convertsSrgbComponentsToLinear()
{
    QCOMPARE(terminalSrgbToLinear(0.0F), 0.0F);
    QCOMPARE(terminalSrgbToLinear(1.0F), 1.0F);
    QVERIFY(std::abs(terminalSrgbToLinear(0.5F) - 0.21404114F) < 0.000001F);
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
