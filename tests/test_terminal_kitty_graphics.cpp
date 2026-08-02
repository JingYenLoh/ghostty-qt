#include "terminal_kitty_graphics.h"
#include "terminal_kitty_graphics_qsg.h"

#include <QFile>
#include <QTest>
#include <rhi/qshader.h>

#include <memory>
#include <ranges>

namespace {

std::shared_ptr<const TerminalKittyGraphicsImage> image(QSize size)
{
    QImage rgba(size, QImage::Format_RGBA8888);
    rgba.fill(QColor(30, 60, 90));
    return std::make_shared<const TerminalKittyGraphicsImage>(
        TerminalKittyGraphicsImage{
            .imageId = 7,
            .generation = 19,
            .fullyOpaque = true,
            .straightRgba = std::move(rgba),
        });
}

void compareReal(qreal actual, qreal expected)
{
    QVERIFY2(qAbs(actual - expected) < 0.0001,
             qPrintable(QStringLiteral("actual %1, expected %2")
                            .arg(actual, 0, 'g', 16)
                            .arg(expected, 0, 'g', 16)));
}

const QShaderDescription::BlockVariable *
memberNamed(const QShaderDescription::UniformBlock &block, QByteArrayView name)
{
    const auto found =
        std::ranges::find_if(block.members, [name](const auto &member) {
            return member.name == name;
        });
    return found == block.members.cend() ? nullptr : &*found;
}

} // namespace

class TerminalKittyGraphicsTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void projectsPhysicalGeometryAndCropsSource();
    void clipsAtRightAndBottomEdges();
    void rejectsInvalidGeometry();
    void embeddedShaderMatchesCpuUniformLayout_data();
    void embeddedShaderMatchesCpuUniformLayout();
};

void TerminalKittyGraphicsTest::projectsPhysicalGeometryAndCropsSource()
{
    const TerminalKittyGraphicsPlacement placement{
        .image = image(QSize(100, 80)),
        .placementId = 11,
        .z = -4,
        .layer = TerminalKittyGraphicsLayer::BelowText,
        .viewportColumn = 2,
        .viewportRow = 1,
        .xOffsetPixels = 4,
        .yOffsetPixels = 6,
        .destinationWidthPixels = 20,
        .destinationHeightPixels = 30,
        .sourceX = 10,
        .sourceY = 20,
        .sourceWidth = 40,
        .sourceHeight = 50,
    };

    const auto rendered = terminalKittyGraphicsRenderPlacement(
        placement, QSizeF(10, 20), QSizeF(8, 16), QRectF(35, 30, 15, 20));
    QVERIFY(rendered.has_value());
    QCOMPARE(rendered->image, placement.image);
    QCOMPARE(rendered->placementId, quint32{11});
    QCOMPARE(rendered->z, qint32{-4});
    QCOMPARE(rendered->layer, TerminalKittyGraphicsLayer::BelowText);
    QCOMPARE(rendered->destination, QRectF(35, 30, 15, 20));
    compareReal(rendered->source.x(), 26.0);
    compareReal(rendered->source.y(), 23.3333333333333);
    compareReal(rendered->source.width(), 24.0);
    compareReal(rendered->source.height(), 26.6666666666667);
}

void TerminalKittyGraphicsTest::clipsAtRightAndBottomEdges()
{
    const TerminalKittyGraphicsPlacement placement{
        .image = image(QSize(40, 30)),
        .viewportColumn = 1,
        .viewportRow = 1,
        .destinationWidthPixels = 20,
        .destinationHeightPixels = 20,
        .sourceX = 5,
        .sourceY = 5,
        .sourceWidth = 20,
        .sourceHeight = 20,
    };

    const auto rendered = terminalKittyGraphicsRenderPlacement(
        placement, QSizeF(10, 10), QSizeF(10, 10), QRectF(0, 0, 25, 25));
    QVERIFY(rendered.has_value());
    QCOMPARE(rendered->destination, QRectF(10, 10, 15, 15));
    QCOMPARE(rendered->source, QRectF(5, 5, 15, 15));

    TerminalKittyGraphicsPlacement outside = placement;
    outside.viewportColumn = 3;
    QVERIFY(!terminalKittyGraphicsRenderPlacement(
                 outside, QSizeF(10, 10), QSizeF(10, 10), QRectF(0, 0, 25, 25))
                 .has_value());
}

void TerminalKittyGraphicsTest::rejectsInvalidGeometry()
{
    TerminalKittyGraphicsPlacement placement{
        .image = image(QSize(20, 20)),
        .destinationWidthPixels = 10,
        .destinationHeightPixels = 10,
        .sourceWidth = 10,
        .sourceHeight = 10,
    };
    const auto render = [&placement] {
        return terminalKittyGraphicsRenderPlacement(
            placement, QSizeF(10, 10), QSizeF(10, 10), QRectF(0, 0, 20, 20));
    };

    QVERIFY(render().has_value());
    placement.sourceX = 15;
    QVERIFY(!render().has_value());
    placement.sourceX = 0;
    placement.destinationWidthPixels = 0;
    QVERIFY(!render().has_value());
    placement.destinationWidthPixels = 10;
    placement.image.reset();
    QVERIFY(!render().has_value());
}

void TerminalKittyGraphicsTest::embeddedShaderMatchesCpuUniformLayout_data()
{
    QTest::addColumn<QString>("resource");
    QTest::addColumn<int>("stage");
    QTest::addColumn<bool>("hasImageSamplers");

    QTest::newRow("vertex")
        << QStringLiteral(":/ghostty-qt/shaders/terminal_kitty.vert.qsb")
        << static_cast<int>(QShader::VertexStage) << false;
    QTest::newRow("fragment")
        << QStringLiteral(":/ghostty-qt/shaders/terminal_kitty.frag.qsb")
        << static_cast<int>(QShader::FragmentStage) << true;
}

void TerminalKittyGraphicsTest::embeddedShaderMatchesCpuUniformLayout()
{
    QFETCH(QString, resource);
    QFETCH(int, stage);
    QFETCH(bool, hasImageSamplers);

    QFile file(resource);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
    const QShader shader = QShader::fromSerialized(file.readAll());
    QVERIFY(shader.isValid());
    QCOMPARE(static_cast<int>(shader.stage()), stage);

    const auto blocks = shader.description().uniformBlocks();
    QCOMPARE(blocks.size(), 1);
    const auto &block = blocks.constFirst();
    QCOMPARE(block.binding, 0);
    QCOMPARE(
        block.size,
        static_cast<int>(TerminalKittyGraphicsShaderLayout::uniformBufferSize));
    QCOMPARE(block.members.size(), 3);

    const auto *matrix = memberNamed(block, QByteArrayView("qt_Matrix"));
    QVERIFY(matrix != nullptr);
    QCOMPARE(matrix->type, QShaderDescription::Mat4);
    QCOMPARE(matrix->offset,
             static_cast<int>(TerminalKittyGraphicsShaderLayout::matrixOffset));
    QCOMPARE(matrix->size,
             static_cast<int>(TerminalKittyGraphicsShaderLayout::matrixSize));
    QCOMPARE(matrix->matrixStride, 16);

    const auto *opacity = memberNamed(block, QByteArrayView("qt_Opacity"));
    QVERIFY(opacity != nullptr);
    QCOMPARE(opacity->type, QShaderDescription::Float);
    QCOMPARE(opacity->offset,
             static_cast<int>(
                 TerminalKittyGraphicsShaderLayout::inheritedOpacityOffset));
    QCOMPARE(opacity->size, static_cast<int>(sizeof(float)));

    const auto *linearBlending =
        memberNamed(block, QByteArrayView("linearBlending"));
    QVERIFY(linearBlending != nullptr);
    QCOMPARE(linearBlending->type, QShaderDescription::Float);
    QCOMPARE(linearBlending->offset,
             static_cast<int>(
                 TerminalKittyGraphicsShaderLayout::linearBlendingOffset));
    QCOMPARE(linearBlending->size, static_cast<int>(sizeof(float)));

    const auto samplers = shader.description().combinedImageSamplers();
    if (!hasImageSamplers) {
        QVERIFY(samplers.isEmpty());
        return;
    }
    QCOMPARE(samplers.size(), 1);
    QCOMPARE(samplers.at(0).name, QByteArray("straightRgba"));
    QCOMPARE(samplers.at(0).binding, 1);
}

QTEST_MAIN(TerminalKittyGraphicsTest)

#include "test_terminal_kitty_graphics.moc"
