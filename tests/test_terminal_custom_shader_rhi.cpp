#include "terminal_custom_shader_compiler.h"
#include "terminal_custom_shader_pipeline.h"
#include "terminal_custom_shader_qsg.h"
#include "terminal_rect_batch.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QTest>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <array>
#include <memory>
#include <utility>

namespace {

QSGRendererInterface::GraphicsApi requestedGraphicsApi =
    QSGRendererInterface::OpenGL;

class TestRectBatchItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool rectVisible READ rectVisible WRITE setRectVisible NOTIFY
                   rectVisibleChanged)

public:
    explicit TestRectBatchItem(QQuickItem *parent = nullptr)
        : QQuickItem(parent)
    {
        setFlag(QQuickItem::ItemHasContents);
    }

    [[nodiscard]] bool rectVisible() const noexcept { return rectVisible_; }

    void setRectVisible(bool visible)
    {
        if (rectVisible_ == visible) return;
        rectVisible_ = visible;
        update();
        Q_EMIT rectVisibleChanged();
    }

Q_SIGNALS:
    void rectVisibleChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override
    {
        auto *batch = static_cast<TerminalRectBatch *>(oldNode);
        if (batch == nullptr) batch = new TerminalRectBatch;
        QVector<TerminalColoredRect> &rects = batch->beginUpdate();
        if (rectVisible_) {
            rects.append({
                .rect = boundingRect(),
                .color = QColor(QStringLiteral("#ff00ff")),
            });
        }
        batch->commit(false);
        return batch;
    }

    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override
    {
        QQuickItem::geometryChange(newGeometry, oldGeometry);
        if (newGeometry.size() != oldGeometry.size()) update();
    }

private:
    bool rectVisible_ = true;
};

class TestUniformProvider final : public QObject,
                                  public TerminalCustomShaderUniformProvider {
    Q_OBJECT
    Q_INTERFACES(TerminalCustomShaderUniformProvider)

public:
    explicit TestUniformProvider(const QColor &color) { setColor(color); }
    explicit TestUniformProvider(const QVector<QColor> &stageColors)
    {
        setColors(stageColors);
    }

    TerminalCustomShaderUniformSnapshot
    terminalCustomShaderUniformSnapshot(int stageIndex) const override
    {
        if (uniforms_.size() == 1) return uniforms_.constFirst();
        return stageIndex >= 0 && stageIndex < uniforms_.size()
            ? uniforms_.at(stageIndex)
            : TerminalCustomShaderUniformSnapshot{};
    }

    void terminalCustomShaderEffectAttached(TerminalCustomShaderEffect *effect,
                                            int) override
    {
        effects_.append(effect);
    }

    void terminalCustomShaderEffectDetached(TerminalCustomShaderEffect *effect,
                                            int) override
    {
        effects_.removeAll(effect);
    }

    void terminalCustomShaderPipelineAttached(
        TerminalCustomShaderPipelineEffect *effect) override
    {
        pipelines_.append(effect);
    }

    void terminalCustomShaderPipelineDetached(
        TerminalCustomShaderPipelineEffect *effect) override
    {
        pipelines_.removeAll(effect);
    }

    [[nodiscard]] TerminalCustomShaderPipelineEffect *pipeline() const
    {
        return pipelines_.isEmpty() ? nullptr : pipelines_.constFirst().data();
    }

    void setColor(const QColor &color) { setColors({color}); }

    void setColors(const QVector<QColor> &stageColors)
    {
        QVector<TerminalCustomShaderUniformSnapshot> uniforms;
        uniforms.reserve(stageColors.size());
        for (const QColor &color : stageColors) {
            auto snapshot = std::make_shared<TerminalCustomShaderUniforms>();
            snapshot->resolution = {32.0F, 32.0F, 1.0F};
            snapshot->backgroundColor = {color.redF(), color.greenF(),
                                         color.blueF(), color.alphaF()};
            uniforms.append(std::move(snapshot));
        }
        uniforms_ = std::move(uniforms);
        for (TerminalCustomShaderEffect *const effect :
             std::as_const(effects_)) {
            if (effect != nullptr) effect->update();
        }
        for (TerminalCustomShaderPipelineEffect *const pipeline :
             std::as_const(pipelines_)) {
            if (pipeline != nullptr) pipeline->update();
        }
    }

private:
    QVector<TerminalCustomShaderUniformSnapshot> uniforms_;
    QVector<QPointer<TerminalCustomShaderEffect>> effects_;
    QVector<QPointer<TerminalCustomShaderPipelineEffect>> pipelines_;
};

bool near(int actual, int expected, int tolerance = 12)
{
    return qAbs(actual - expected) <= tolerance;
}

bool isMostly(const QColor &actual, const QColor &expected)
{
    return near(actual.red(), expected.red())
        && near(actual.green(), expected.green())
        && near(actual.blue(), expected.blue());
}

QString colorDescription(const QColor &color)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QImage renderWindow(QQuickWindow &window)
{
    window.show();
    if (!QTest::qWaitForWindowExposed(&window, 5'000)) return {};

    QImage frame;
    for (int attempt = 0; attempt < 100 && frame.isNull(); ++attempt) {
        frame = window.grabWindow();
        if (frame.isNull()) QTest::qWait(20);
    }
    return frame;
}

TerminalCustomShaderStage compileShader(const QString &source,
                                        const QString &name,
                                        const QDir &directory,
                                        QString *diagnostic)
{
    const QString sourcePath = directory.filePath(name);
    QFile sourceFile(sourcePath);
    if (!sourceFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *diagnostic = sourceFile.errorString();
        return {};
    }
    const QByteArray sourceBytes = source.toUtf8();
    if (sourceFile.write(sourceBytes) != sourceBytes.size()) {
        *diagnostic = sourceFile.errorString();
        return {};
    }
    sourceFile.close();

    TerminalCustomShaderOptions options;
    options.sources.append({.path = sourcePath, .optional = false});
    TerminalCustomShaderCompileResult result = compileTerminalCustomShaders(
        options, directory.filePath(QStringLiteral("cache")));
    if (!result.succeeded()) {
        *diagnostic = result.diagnostic;
        return {};
    }
    if (result.stages.size() != 1) {
        *diagnostic = QStringLiteral("expected one stage, received %1")
                          .arg(result.stages.size());
        return {};
    }
    return result.stages.constFirst();
}

QColor orderedExpectedColor(int passCount)
{
    const QColor source(QStringLiteral("#204060"));
    std::array<qreal, 3> color{
        source.redF(),
        source.greenF(),
        source.blueF(),
    };
    if (passCount >= 1) color = {color[1], color[2], color[0]};
    if (passCount >= 2) {
        color = {
            color[0] * 0.5 + 0.1,
            color[1] * 0.75 + 0.05,
            color[2] * 0.25 + 0.2,
        };
    }
    if (passCount >= 3) {
        color = {
            1.0 - color[0],
            1.0 - color[1],
            1.0 - color[2],
        };
    }
    if (passCount >= 4) color = {color[2], color[0], color[1]};
    return QColor::fromRgbF(color[0], color[1], color[2]);
}

QColor compositeColor(const QColor &source, qreal opacity,
                      const QColor &background)
{
    const qreal sourceAlpha = source.alphaF() * opacity;
    const qreal backgroundContribution =
        background.alphaF() * (1.0 - sourceAlpha);
    const qreal outputAlpha = sourceAlpha + backgroundContribution;
    const auto component = [&](qreal sourceComponent,
                               qreal backgroundComponent) {
        if (outputAlpha <= 0.0) return 0.0;
        return (sourceComponent * sourceAlpha
                + backgroundComponent * backgroundContribution)
            / outputAlpha;
    };
    return QColor::fromRgbF(component(source.redF(), background.redF()),
                            component(source.greenF(), background.greenF()),
                            component(source.blueF(), background.blueF()),
                            outputAlpha);
}

} // namespace

class TerminalCustomShaderRhiTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void identityShaderPreservesVerticalOrientation();
    void sharedProgramKeepsPerMaterialUniformsIsolated();
    void retainedIdentityPreservesAsymmetricOrientation();
    void retainedOrderedMultipass_data();
    void retainedOrderedMultipass();
    void retainedStageSpecificUniformsRemainDistinct();
    void retainedSourceFramesAreFresh();
    void retainedRectBatchRecoversAfterEmptyFrames();
    void retainedSourceTextureSwapReusesBindings();
    void retainedSharedProgramKeepsPerProviderUniformsIsolated();
    void retainedTargetsAreBoundedAndReused_data();
    void retainedTargetsAreBoundedAndReused();
    void retainedLinearModeUsesFloatTargetsAndReloads();
    void retainedResizeKeepsPipelinesAndUpdatesGeometry();
    void retainedParentOpacityCompositesPremultipliedContent();
    void retainedRectangularClipUsesScissor();
    void retainedRotatedRectangularClipUsesStencil();

private:
    std::unique_ptr<QTemporaryDir> shaderRoot_;
    TerminalCustomShaderStage identityStage_;
    TerminalCustomShaderStage uniformStage_;
    TerminalCustomShaderStage uniformMixStage_;
    QVector<TerminalCustomShaderStage> orderedStages_;
};

void TerminalCustomShaderRhiTest::initTestCase()
{
    if (requestedGraphicsApi == QSGRendererInterface::OpenGL) {
        std::unique_ptr<QOffscreenSurface> fallbackSurface(
            QRhiGles2InitParams::newFallbackSurface());
        QRhiGles2InitParams params;
        params.fallbackSurface = fallbackSurface.get();
        std::unique_ptr<QRhi> rhi(fallbackSurface != nullptr
                                          && fallbackSurface->isValid()
                                      ? QRhi::create(QRhi::OpenGLES2, &params)
                                      : nullptr);
        if (rhi == nullptr) {
            QSKIP("The platform cannot initialize the requested RHI context.");
        }
    }

    qmlRegisterType<TerminalCustomShaderEffect>("GhosttyQtShaderRhiTest", 1, 0,
                                                "TerminalCustomShaderEffect");
    qmlRegisterType<TerminalCustomShaderPipelineEffect>(
        "GhosttyQtShaderRhiTest", 1, 0, "TerminalCustomShaderPipelineEffect");
    qmlRegisterType<TestRectBatchItem>("GhosttyQtShaderRhiTest", 1, 0,
                                       "TestRectBatchItem");

    shaderRoot_ = std::make_unique<QTemporaryDir>();
    QVERIFY(shaderRoot_->isValid());
    const QDir directory(shaderRoot_->path());

    QString diagnostic;
    identityStage_ =
        compileShader(QStringLiteral(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = texture(iChannel0, fragCoord / iResolution.xy);
}
)glsl"),
                      QStringLiteral("identity.glsl"), directory, &diagnostic);
    QVERIFY2(!identityStage_.qsbPath.isEmpty(), qPrintable(diagnostic));

    uniformStage_ =
        compileShader(QStringLiteral(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = vec4(iBackgroundColor, 1.0);
}
)glsl"),
                      QStringLiteral("uniform.glsl"), directory, &diagnostic);
    QVERIFY2(!uniformStage_.qsbPath.isEmpty(), qPrintable(diagnostic));

    uniformMixStage_ = compileShader(QStringLiteral(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    vec4 source = texture(iChannel0, fragCoord / iResolution.xy);
    color = vec4(source.rgb * 0.5 + iBackgroundColor * 0.5, source.a);
}
)glsl"),
                                     QStringLiteral("uniform-mix.glsl"),
                                     directory, &diagnostic);
    QVERIFY2(!uniformMixStage_.qsbPath.isEmpty(), qPrintable(diagnostic));

    const QStringList orderedSources{
        QStringLiteral(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    vec4 source = texture(iChannel0, fragCoord / iResolution.xy);
    color = vec4(source.g, source.b, source.r, source.a);
}
)glsl"),
        QStringLiteral(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    vec4 source = texture(iChannel0, fragCoord / iResolution.xy);
    color = vec4(source.r * 0.5 + 0.1,
                 source.g * 0.75 + 0.05,
                 source.b * 0.25 + 0.2,
                 source.a);
}
)glsl"),
        QStringLiteral(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    vec4 source = texture(iChannel0, fragCoord / iResolution.xy);
    color = vec4(vec3(1.0) - source.rgb, source.a);
}
)glsl"),
        QStringLiteral(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    vec4 source = texture(iChannel0, fragCoord / iResolution.xy);
    color = vec4(source.b, source.r, source.g, source.a);
}
)glsl"),
    };
    orderedStages_.reserve(orderedSources.size());
    for (qsizetype index = 0; index < orderedSources.size(); ++index) {
        TerminalCustomShaderStage stage =
            compileShader(orderedSources.at(index),
                          QStringLiteral("ordered-%1.glsl").arg(index + 1),
                          directory, &diagnostic);
        QVERIFY2(!stage.qsbPath.isEmpty(), qPrintable(diagnostic));
        orderedStages_.append(std::move(stage));
    }
}

void TerminalCustomShaderRhiTest::identityShaderPreservesVerticalOrientation()
{
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("shaderPath"),
                                             identityStage_.qsbPath);
    engine.rootContext()->setContextProperty(QStringLiteral("shaderData"),
                                             identityStage_.serializedShader);
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 32

    Item {
        anchors.fill: parent
        layer.enabled: true
        layer.live: true
        layer.smooth: true
        layer.textureMirroring: ShaderEffectSource.NoMirroring
        layer.textureSize: Qt.size(width, height)
        layer.effect: Component {
            TerminalCustomShaderEffect {
                fragmentShaderFileName: shaderPath
                fragmentShaderData: shaderData
                uniformProvider: testUniformProvider
                stageIndex: 0
            }
        }

        Rectangle {
            width: parent.width
            height: parent.height / 2
            color: "#f02010"
        }
        Rectangle {
            y: parent.height / 2
            width: parent.width
            height: parent.height / 2
            color: "#1040f0"
        }
    }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/custom-shader-identity.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    const QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(), "OpenGL RHI produced no frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    QVERIFY(QSGRendererInterface::isApiRhiBased(
        window.rendererInterface()->graphicsApi()));

    const QColor top = frame.pixelColor(frame.width() / 2, frame.height() / 4);
    const QColor bottom =
        frame.pixelColor(frame.width() / 2, frame.height() * 3 / 4);
    QVERIFY2(isMostly(top, QColor(QStringLiteral("#f02010"))),
             qPrintable(QStringLiteral("unexpected top sample %1")
                            .arg(colorDescription(top))));
    QVERIFY2(isMostly(bottom, QColor(QStringLiteral("#1040f0"))),
             qPrintable(QStringLiteral("unexpected bottom sample %1")
                            .arg(colorDescription(bottom))));

    root.reset();
}

void TerminalCustomShaderRhiTest::
    sharedProgramKeepsPerMaterialUniformsIsolated()
{
    TestUniformProvider leftProvider(Qt::red);
    TestUniformProvider rightProvider(Qt::green);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("shaderPath"),
                                             uniformStage_.qsbPath);
    engine.rootContext()->setContextProperty(QStringLiteral("shaderData"),
                                             uniformStage_.serializedShader);
    engine.rootContext()->setContextProperty(QStringLiteral("leftProvider"),
                                             &leftProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("rightProvider"),
                                             &rightProvider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 64
    height: 32

    Item {
        width: 32
        height: 32
        layer.enabled: true
        layer.live: true
        layer.smooth: true
        layer.textureMirroring: ShaderEffectSource.NoMirroring
        layer.textureSize: Qt.size(width, height)
        layer.effect: Component {
            TerminalCustomShaderEffect {
                fragmentShaderFileName: shaderPath
                fragmentShaderData: shaderData
                uniformProvider: leftProvider
                stageIndex: 0
            }
        }
        Rectangle { anchors.fill: parent; color: "white" }
    }

    Item {
        x: 32
        width: 32
        height: 32
        layer.enabled: true
        layer.live: true
        layer.smooth: true
        layer.textureMirroring: ShaderEffectSource.NoMirroring
        layer.textureSize: Qt.size(width, height)
        layer.effect: Component {
            TerminalCustomShaderEffect {
                fragmentShaderFileName: shaderPath
                fragmentShaderData: shaderData
                uniformProvider: rightProvider
                stageIndex: 0
            }
        }
        Rectangle { anchors.fill: parent; color: "white" }
    }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/custom-shader-uniforms.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(64, 32);
    root->setParentItem(window.contentItem());
    QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(), "OpenGL RHI produced no frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    QColor left = frame.pixelColor(frame.width() / 4, frame.height() / 2);
    QColor right = frame.pixelColor(frame.width() * 3 / 4, frame.height() / 2);
    QVERIFY2(isMostly(left, Qt::red),
             qPrintable(QStringLiteral("unexpected left sample %1")
                            .arg(colorDescription(left))));
    QVERIFY2(isMostly(right, Qt::green),
             qPrintable(QStringLiteral("unexpected right sample %1")
                            .arg(colorDescription(right))));

    leftProvider.setColor(Qt::blue);
    rightProvider.setColor(Qt::yellow);
    frame = window.grabWindow();
    QVERIFY2(!frame.isNull(), "OpenGL RHI produced no updated frame.");
    left = frame.pixelColor(frame.width() / 4, frame.height() / 2);
    right = frame.pixelColor(frame.width() * 3 / 4, frame.height() / 2);
    QVERIFY2(isMostly(left, Qt::blue),
             qPrintable(QStringLiteral("unexpected updated left sample %1")
                            .arg(colorDescription(left))));
    QVERIFY2(isMostly(right, Qt::yellow),
             qPrintable(QStringLiteral("unexpected updated right sample %1")
                            .arg(colorDescription(right))));

    root.reset();
}

void TerminalCustomShaderRhiTest::
    retainedIdentityPreservesAsymmetricOrientation()
{
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList({identityStage_}));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 32
    layer.enabled: true
    layer.live: true
    layer.smooth: false
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.textureSize: Qt.size(width, height)
    layer.effect: Component {
        TerminalCustomShaderPipelineEffect {
            objectName: "pipeline"
            shaderStages: pipelineStages
            uniformProvider: testUniformProvider
        }
    }

    Rectangle { width: 16; height: 16; color: "#e02010" }
    Rectangle { x: 16; width: 16; height: 16; color: "#10d040" }
    Rectangle { y: 16; width: 16; height: 16; color: "#2040e0" }
    Rectangle {
        x: 16
        y: 16
        width: 16
        height: 16
        color: "#d0b020"
    }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/custom-shader-retained-identity.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    const QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(), "Retained RHI pipeline produced no frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    struct Sample {
        QPoint point;
        QColor expected;
        const char *name;
    };
    const std::array<Sample, 4> samples{{
        {{8, 8}, QColor(QStringLiteral("#e02010")), "top-left"},
        {{24, 8}, QColor(QStringLiteral("#10d040")), "top-right"},
        {{8, 24}, QColor(QStringLiteral("#2040e0")), "bottom-left"},
        {{24, 24}, QColor(QStringLiteral("#d0b020")), "bottom-right"},
    }};
    for (const Sample &sample : samples) {
        const QColor actual = frame.pixelColor(sample.point);
        QVERIFY2(isMostly(actual, sample.expected),
                 qPrintable(QStringLiteral("unexpected %1 sample %2")
                                .arg(QString::fromUtf8(sample.name),
                                     colorDescription(actual))));
    }

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    QCOMPARE(pipeline->renderSnapshot().passCount, 1);
    QCOMPARE(pipeline->renderSnapshot().uniformSlotCount, 1);
    QCOMPARE(
        pipeline->renderSnapshot().uniformUploadBytesPerFrame,
        static_cast<std::uint64_t>(TerminalCustomShaderUniformLayout::size));
    QVERIFY2(pipeline->renderDiagnostic().isEmpty(),
             qPrintable(pipeline->renderDiagnostic()));

    root.reset();
}

void TerminalCustomShaderRhiTest::retainedOrderedMultipass_data()
{
    QTest::addColumn<int>("passCount");
    QTest::newRow("odd-three-pass") << 3;
    QTest::newRow("even-four-pass") << 4;
}

void TerminalCustomShaderRhiTest::retainedOrderedMultipass()
{
    QFETCH(int, passCount);
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList(
            orderedStages_.mid(0, passCount)));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 32
    layer.enabled: true
    layer.live: true
    layer.smooth: false
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.textureSize: Qt.size(width, height)
    layer.effect: Component {
        TerminalCustomShaderPipelineEffect {
            objectName: "pipeline"
            shaderStages: pipelineStages
            uniformProvider: testUniformProvider
        }
    }
    Rectangle { anchors.fill: parent; color: "#204060" }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/custom-shader-retained-ordered.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    const QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(), "Retained RHI pipeline produced no frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    const QColor actual =
        frame.pixelColor(frame.width() / 2, frame.height() / 2);
    const QColor expected = orderedExpectedColor(passCount);
    QVERIFY2(
        isMostly(actual, expected),
        qPrintable(
            QStringLiteral("unexpected ordered %1-pass sample %2; expected %3")
                .arg(passCount)
                .arg(colorDescription(actual), colorDescription(expected))));

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    const TerminalCustomShaderPipelineSnapshot snapshot =
        pipeline->renderSnapshot();
    QCOMPARE(snapshot.passCount, passCount);
    QCOMPARE(snapshot.liveTargetCount, 2);
    QCOMPARE(snapshot.liveBindingCount,
             terminalCustomShaderPipelineBindingCount(passCount));
    QCOMPARE(snapshot.uniformSlotCount, 2);
    QCOMPARE(snapshot.uniformUploadBytesPerFrame,
             static_cast<std::uint64_t>(
                 2 * TerminalCustomShaderUniformLayout::size));
    QVERIFY(snapshot.uniformBufferBytes >= snapshot.uniformUploadBytesPerFrame);
    QVERIFY2(snapshot.diagnostic.isEmpty(), qPrintable(snapshot.diagnostic));

    root.reset();
}

void TerminalCustomShaderRhiTest::retainedStageSpecificUniformsRemainDistinct()
{
    TestUniformProvider provider(QVector<QColor>{Qt::red, Qt::green, Qt::blue});
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList(
            {uniformMixStage_, uniformMixStage_, uniformMixStage_}));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 32
    layer.enabled: true
    layer.live: true
    layer.smooth: false
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.textureSize: Qt.size(width, height)
    layer.effect: Component {
        TerminalCustomShaderPipelineEffect {
            shaderStages: pipelineStages
            uniformProvider: testUniformProvider
        }
    }
    Rectangle { anchors.fill: parent; color: "black" }
}
)qml",
        QUrl(QStringLiteral(
            "qrc:/test/custom-shader-retained-stage-uniforms.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no stage-uniform frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    const QColor actual =
        frame.pixelColor(frame.width() / 2, frame.height() / 2);
    const QColor expected = QColor::fromRgb(32, 64, 128);
    QVERIFY2(isMostly(actual, expected),
             qPrintable(QStringLiteral("unexpected stage-uniform sample %1; "
                                       "expected %2")
                            .arg(colorDescription(actual),
                                 colorDescription(expected))));

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    const TerminalCustomShaderPipelineSnapshot distinctSnapshot =
        pipeline->renderSnapshot();
    QCOMPARE(distinctSnapshot.passCount, 3);
    QCOMPARE(distinctSnapshot.uniformSlotCount, 3);
    QCOMPARE(distinctSnapshot.uniformUploadBytesPerFrame,
             static_cast<std::uint64_t>(
                 3 * TerminalCustomShaderUniformLayout::size));
    QVERIFY(distinctSnapshot.uniformBufferBytes
            >= distinctSnapshot.uniformUploadBytesPerFrame);
    QVERIFY2(distinctSnapshot.diagnostic.isEmpty(),
             qPrintable(distinctSnapshot.diagnostic));

    provider.setColor(Qt::yellow);
    frame = window.grabWindow();
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no shared-uniform frame.");
    const QColor sharedActual =
        frame.pixelColor(frame.width() / 2, frame.height() / 2);
    const QColor sharedExpected = QColor::fromRgb(223, 223, 0);
    QVERIFY2(isMostly(sharedActual, sharedExpected),
             qPrintable(QStringLiteral("unexpected shared-uniform sample %1; "
                                       "expected %2")
                            .arg(colorDescription(sharedActual),
                                 colorDescription(sharedExpected))));

    const TerminalCustomShaderPipelineSnapshot sharedSnapshot =
        pipeline->renderSnapshot();
    QCOMPARE(sharedSnapshot.uniformSlotCount, 2);
    QCOMPARE(sharedSnapshot.uniformUploadBytesPerFrame,
             static_cast<std::uint64_t>(
                 2 * TerminalCustomShaderUniformLayout::size));
    QVERIFY(sharedSnapshot.uniformBufferBytes
            >= sharedSnapshot.uniformUploadBytesPerFrame);
    QVERIFY(sharedSnapshot.uniformBufferBytes
            < distinctSnapshot.uniformBufferBytes);
    QVERIFY2(sharedSnapshot.diagnostic.isEmpty(),
             qPrintable(sharedSnapshot.diagnostic));

    root.reset();
}

void TerminalCustomShaderRhiTest::retainedSourceFramesAreFresh()
{
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList(
            {identityStage_, identityStage_}));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 32
    layer.enabled: true
    layer.live: true
    layer.smooth: false
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.textureSize: Qt.size(width, height)
    layer.effect: Component {
        TerminalCustomShaderPipelineEffect {
            objectName: "pipeline"
            shaderStages: pipelineStages
            uniformProvider: testUniformProvider
        }
    }
    Rectangle {
        objectName: "sourceRect"
        anchors.fill: parent
        color: "#d02040"
    }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/custom-shader-retained-freshness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(), "Retained RHI pipeline produced no first frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    QColor actual = frame.pixelColor(frame.width() / 2, frame.height() / 2);
    QVERIFY2(isMostly(actual, QColor(QStringLiteral("#d02040"))),
             qPrintable(QStringLiteral("stale first-frame sample %1")
                            .arg(colorDescription(actual))));

    auto *const sourceRect =
        root->findChild<QQuickItem *>(QStringLiteral("sourceRect"));
    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(sourceRect != nullptr);
    QVERIFY(pipeline != nullptr);
    const TerminalCustomShaderPipelineSnapshot before =
        pipeline->renderSnapshot();

    const QColor updated(QStringLiteral("#20c060"));
    QVERIFY(sourceRect->setProperty("color", updated));
    window.update();
    frame = window.grabWindow();
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no source-update frame.");
    actual = frame.pixelColor(frame.width() / 2, frame.height() / 2);
    QVERIFY2(isMostly(actual, updated),
             qPrintable(QStringLiteral("stale source-update sample %1")
                            .arg(colorDescription(actual))));

    const TerminalCustomShaderPipelineSnapshot after =
        pipeline->renderSnapshot();
    QVERIFY(after.frameCount > before.frameCount);
    QCOMPARE(after.targetCreateCount, before.targetCreateCount);
    QCOMPARE(after.resourceGeneration, before.resourceGeneration);

    root.reset();
}

void TerminalCustomShaderRhiTest::retainedRectBatchRecoversAfterEmptyFrames()
{
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList({identityStage_}));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 32
    layer.enabled: true
    layer.live: true
    layer.smooth: false
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.textureSize: Qt.size(width, height)
    layer.effect: Component {
        TerminalCustomShaderPipelineEffect {
            objectName: "pipeline"
            shaderStages: pipelineStages
            uniformProvider: testUniformProvider
        }
    }
    TestRectBatchItem {
        objectName: "sourceBatch"
        anchors.fill: parent
    }
}
)qml",
        QUrl(
            QStringLiteral("qrc:/test/custom-shader-retained-rect-batch.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no first batch frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    auto *const sourceBatch =
        root->findChild<TestRectBatchItem *>(QStringLiteral("sourceBatch"));
    QVERIFY(sourceBatch != nullptr);
    const auto sample = [&window] {
        const QImage image = window.grabWindow();
        return image.isNull()
            ? QColor{}
            : image.pixelColor(image.width() / 2, image.height() / 2);
    };
    QVERIFY2(isMostly(frame.pixelColor(frame.width() / 2, frame.height() / 2),
                      QColor(QStringLiteral("#ff00ff"))),
             "Retained RHI pipeline dropped the initial rectangle batch.");

    for (int cycle = 0; cycle < 2; ++cycle) {
        sourceBatch->setRectVisible(false);
        window.update();
        const QColor hidden = sample();
        QVERIFY2(
            isMostly(hidden, Qt::black),
            qPrintable(
                QStringLiteral("stale visible rectangle in hidden cycle %1: %2")
                    .arg(cycle + 1)
                    .arg(colorDescription(hidden))));

        sourceBatch->setRectVisible(true);
        window.update();
        const QColor visible = sample();
        QVERIFY2(
            isMostly(visible, QColor(QStringLiteral("#ff00ff"))),
            qPrintable(QStringLiteral(
                           "rectangle batch did not recover in cycle %1: %2")
                           .arg(cycle + 1)
                           .arg(colorDescription(visible))));
    }

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    QVERIFY2(pipeline->renderDiagnostic().isEmpty(),
             qPrintable(pipeline->renderDiagnostic()));
    root.reset();
}

void TerminalCustomShaderRhiTest::retainedSourceTextureSwapReusesBindings()
{
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList(
            {identityStage_, identityStage_, identityStage_, identityStage_}));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    id: root
    width: 32
    height: 32
    property bool useSecond: false

    Rectangle {
        id: firstRect
        anchors.fill: parent
        color: "#d02040"
    }
    Rectangle {
        id: secondRect
        anchors.fill: parent
        color: "#2040d0"
    }
    ShaderEffectSource {
        id: firstSource
        anchors.fill: parent
        sourceItem: firstRect
        hideSource: true
        live: true
        smooth: false
        textureMirroring: ShaderEffectSource.NoMirroring
        textureSize: Qt.size(width, height)
    }
    ShaderEffectSource {
        id: secondSource
        anchors.fill: parent
        sourceItem: secondRect
        hideSource: true
        live: true
        smooth: false
        textureMirroring: ShaderEffectSource.NoMirroring
        textureSize: Qt.size(width, height)
    }
    TerminalCustomShaderPipelineEffect {
        objectName: "pipeline"
        anchors.fill: parent
        source: root.useSecond ? secondSource : firstSource
        shaderStages: pipelineStages
        uniformProvider: testUniformProvider
    }
}
)qml",
        QUrl(QStringLiteral(
            "qrc:/test/custom-shader-retained-source-swap.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no source-swap frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    QColor actual = frame.pixelColor(frame.width() / 2, frame.height() / 2);
    QVERIFY2(isMostly(actual, QColor(QStringLiteral("#d02040"))),
             qPrintable(QStringLiteral("unexpected first source sample %1")
                            .arg(colorDescription(actual))));

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    const TerminalCustomShaderPipelineSnapshot before =
        pipeline->renderSnapshot();
    QVERIFY2(before.diagnostic.isEmpty(), qPrintable(before.diagnostic));
    QCOMPARE(before.liveBindingCount, 3);
    QCOMPARE(before.bindingCreateCount, std::uint64_t{3});

    QVERIFY(root->setProperty("useSecond", true));
    window.update();
    frame = window.grabWindow();
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no updated source-swap frame.");
    actual = frame.pixelColor(frame.width() / 2, frame.height() / 2);
    QVERIFY2(isMostly(actual, QColor(QStringLiteral("#2040d0"))),
             qPrintable(QStringLiteral("unexpected second source sample %1")
                            .arg(colorDescription(actual))));

    const TerminalCustomShaderPipelineSnapshot after =
        pipeline->renderSnapshot();
    QVERIFY2(after.diagnostic.isEmpty(), qPrintable(after.diagnostic));
    QCOMPARE(after.sourceBindingUpdateCount,
             before.sourceBindingUpdateCount + 1);
    QCOMPARE(after.liveBindingCount, before.liveBindingCount);
    QCOMPARE(after.bindingCreateCount, before.bindingCreateCount);
    QCOMPARE(after.pipelineCreateCount, before.pipelineCreateCount);
    QCOMPARE(after.targetCreateCount, before.targetCreateCount);
    QCOMPARE(after.targetDestroyCount, before.targetDestroyCount);
    QCOMPARE(after.resourceGeneration, before.resourceGeneration);

    QVERIFY(root->setProperty("useSecond", false));
    window.update();
    frame = window.grabWindow();
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no restored source-swap frame.");
    actual = frame.pixelColor(frame.width() / 2, frame.height() / 2);
    QVERIFY2(isMostly(actual, QColor(QStringLiteral("#d02040"))),
             qPrintable(QStringLiteral("unexpected restored source sample %1")
                            .arg(colorDescription(actual))));

    const TerminalCustomShaderPipelineSnapshot restored =
        pipeline->renderSnapshot();
    QVERIFY2(restored.diagnostic.isEmpty(), qPrintable(restored.diagnostic));
    QCOMPARE(restored.sourceBindingUpdateCount,
             before.sourceBindingUpdateCount + 2);
    QCOMPARE(restored.liveBindingCount, before.liveBindingCount);
    QCOMPARE(restored.bindingCreateCount, before.bindingCreateCount);
    QCOMPARE(restored.pipelineCreateCount, before.pipelineCreateCount);
    QCOMPARE(restored.targetCreateCount, before.targetCreateCount);
    QCOMPARE(restored.targetDestroyCount, before.targetDestroyCount);
    QCOMPARE(restored.resourceGeneration, before.resourceGeneration);

    root.reset();
}

void TerminalCustomShaderRhiTest::
    retainedSharedProgramKeepsPerProviderUniformsIsolated()
{
    TestUniformProvider leftProvider(Qt::red);
    TestUniformProvider rightProvider(Qt::green);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList({uniformStage_}));
    engine.rootContext()->setContextProperty(QStringLiteral("leftProvider"),
                                             &leftProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("rightProvider"),
                                             &rightProvider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 64
    height: 32

    Item {
        width: 32
        height: 32
        layer.enabled: true
        layer.live: true
        layer.smooth: false
        layer.textureMirroring: ShaderEffectSource.NoMirroring
        layer.textureSize: Qt.size(width, height)
        layer.effect: Component {
            TerminalCustomShaderPipelineEffect {
                shaderStages: pipelineStages
                uniformProvider: leftProvider
            }
        }
        Rectangle { anchors.fill: parent; color: "white" }
    }

    Item {
        x: 32
        width: 32
        height: 32
        layer.enabled: true
        layer.live: true
        layer.smooth: false
        layer.textureMirroring: ShaderEffectSource.NoMirroring
        layer.textureSize: Qt.size(width, height)
        layer.effect: Component {
            TerminalCustomShaderPipelineEffect {
                shaderStages: pipelineStages
                uniformProvider: rightProvider
            }
        }
        Rectangle { anchors.fill: parent; color: "white" }
    }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/custom-shader-retained-uniforms.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(64, 32);
    root->setParentItem(window.contentItem());
    QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(), "Retained RHI pipeline produced no frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    QColor left = frame.pixelColor(frame.width() / 4, frame.height() / 2);
    QColor right = frame.pixelColor(frame.width() * 3 / 4, frame.height() / 2);
    QVERIFY2(isMostly(left, Qt::red),
             qPrintable(QStringLiteral("unexpected retained left sample %1")
                            .arg(colorDescription(left))));
    QVERIFY2(isMostly(right, Qt::green),
             qPrintable(QStringLiteral("unexpected retained right sample %1")
                            .arg(colorDescription(right))));

    leftProvider.setColor(Qt::blue);
    rightProvider.setColor(Qt::yellow);
    frame = window.grabWindow();
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no updated frame.");
    left = frame.pixelColor(frame.width() / 4, frame.height() / 2);
    right = frame.pixelColor(frame.width() * 3 / 4, frame.height() / 2);
    QVERIFY2(
        isMostly(left, Qt::blue),
        qPrintable(QStringLiteral("unexpected retained updated left sample %1")
                       .arg(colorDescription(left))));
    QVERIFY2(
        isMostly(right, Qt::yellow),
        qPrintable(QStringLiteral("unexpected retained updated right sample %1")
                       .arg(colorDescription(right))));

    root.reset();
}

void TerminalCustomShaderRhiTest::retainedTargetsAreBoundedAndReused_data()
{
    QTest::addColumn<int>("passCount");
    QTest::addColumn<int>("expectedTargets");
    QTest::newRow("one-pass") << 1 << 0;
    QTest::newRow("two-pass") << 2 << 1;
    QTest::newRow("three-pass") << 3 << 2;
    QTest::newRow("four-pass") << 4 << 2;
}

void TerminalCustomShaderRhiTest::retainedTargetsAreBoundedAndReused()
{
    QFETCH(int, passCount);
    QFETCH(int, expectedTargets);
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList(
            orderedStages_.mid(0, passCount)));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 32
    layer.enabled: true
    layer.live: true
    layer.smooth: false
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.textureSize: Qt.size(width, height)
    layer.effect: Component {
        TerminalCustomShaderPipelineEffect {
            objectName: "pipeline"
            shaderStages: pipelineStages
            uniformProvider: testUniformProvider
        }
    }
    Rectangle { anchors.fill: parent; color: "#204060" }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/custom-shader-retained-targets.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    const QImage firstFrame = renderWindow(window);
    QVERIFY2(!firstFrame.isNull(),
             "Retained RHI pipeline produced no first frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    const TerminalCustomShaderPipelineSnapshot first =
        pipeline->renderSnapshot();
    QVERIFY2(first.diagnostic.isEmpty(), qPrintable(first.diagnostic));
    QCOMPARE(first.passCount, passCount);
    QCOMPARE(first.liveTargetCount, expectedTargets);
    const int expectedBindings =
        terminalCustomShaderPipelineBindingCount(passCount);
    QCOMPARE(first.liveBindingCount, expectedBindings);
    QCOMPARE(first.bindingCreateCount,
             static_cast<std::uint64_t>(expectedBindings));
    QCOMPARE(first.targetCreateCount,
             static_cast<std::uint64_t>(expectedTargets));
    QCOMPARE(first.targetPixelSize, QSize(32, 32));
    QCOMPARE(first.ownedTextureBytes,
             static_cast<std::uint64_t>(expectedTargets * 32 * 32 * 4));
    QVERIFY(first.frameCount >= 1);
    QVERIFY(first.drawCount >= static_cast<std::uint64_t>(passCount));

    pipeline->update();
    window.update();
    const QImage secondFrame = window.grabWindow();
    QVERIFY2(!secondFrame.isNull(),
             "Retained RHI pipeline produced no reuse frame.");
    const TerminalCustomShaderPipelineSnapshot second =
        pipeline->renderSnapshot();
    QVERIFY2(second.diagnostic.isEmpty(), qPrintable(second.diagnostic));
    QCOMPARE(second.liveTargetCount, first.liveTargetCount);
    QCOMPARE(second.targetCreateCount, first.targetCreateCount);
    QCOMPARE(second.targetDestroyCount, first.targetDestroyCount);
    QCOMPARE(second.resourceGeneration, first.resourceGeneration);
    QCOMPARE(second.pipelineCreateCount, first.pipelineCreateCount);
    QCOMPARE(second.liveBindingCount, first.liveBindingCount);
    QCOMPARE(second.bindingCreateCount, first.bindingCreateCount);
    QCOMPARE(second.sourceBindingUpdateCount, first.sourceBindingUpdateCount);
    QVERIFY(second.frameCount > first.frameCount);
    QVERIFY(second.drawCount
            >= first.drawCount + static_cast<std::uint64_t>(passCount));

    root.reset();
}

void TerminalCustomShaderRhiTest::retainedLinearModeUsesFloatTargetsAndReloads()
{
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList(orderedStages_.mid(0, 3)));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 32
    layer.enabled: true
    layer.live: true
    layer.smooth: false
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.textureSize: Qt.size(width, height)
    layer.effect: Component {
        TerminalCustomShaderPipelineEffect {
            objectName: "pipeline"
            shaderStages: pipelineStages
            uniformProvider: testUniformProvider
        }
    }
    Rectangle { anchors.fill: parent; color: "#204060" }
}
)qml",
        QUrl(QStringLiteral(
            "qrc:/test/custom-shader-retained-linear-reload.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    QVERIFY(!renderWindow(window).isNull());
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    const TerminalCustomShaderPipelineSnapshot native =
        pipeline->renderSnapshot();
    QCOMPARE(native.liveTargetCount, 2);
    QCOMPARE(native.ownedTextureBytes, std::uint64_t{2 * 32 * 32 * 4});

    pipeline->setLinearBlending(true);
    window.update();
    QVERIFY(!window.grabWindow().isNull());
    const TerminalCustomShaderPipelineSnapshot linear =
        pipeline->renderSnapshot();
    QCOMPARE(linear.liveTargetCount, 2);
    QCOMPARE(linear.ownedTextureBytes, std::uint64_t{2 * 32 * 32 * 8});
    QVERIFY(linear.targetCreateCount > native.targetCreateCount);
    QVERIFY(linear.targetDestroyCount > native.targetDestroyCount);
    QVERIFY(linear.resourceGeneration > native.resourceGeneration);
    QVERIFY2(linear.diagnostic.isEmpty(), qPrintable(linear.diagnostic));

    pipeline->setLinearBlending(false);
    window.update();
    QVERIFY(!window.grabWindow().isNull());
    const TerminalCustomShaderPipelineSnapshot restored =
        pipeline->renderSnapshot();
    QCOMPARE(restored.liveTargetCount, 2);
    QCOMPARE(restored.ownedTextureBytes, std::uint64_t{2 * 32 * 32 * 4});
    QVERIFY(restored.targetCreateCount > linear.targetCreateCount);
    QVERIFY(restored.targetDestroyCount > linear.targetDestroyCount);
    QVERIFY(restored.resourceGeneration > linear.resourceGeneration);
    QVERIFY2(restored.diagnostic.isEmpty(), qPrintable(restored.diagnostic));

    root.reset();
}

void TerminalCustomShaderRhiTest::
    retainedResizeKeepsPipelinesAndUpdatesGeometry()
{
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList(
            {identityStage_, identityStage_, identityStage_}));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 24
    layer.enabled: true
    layer.live: true
    layer.smooth: false
    layer.textureMirroring: ShaderEffectSource.NoMirroring
    layer.textureSize: Qt.size(width, height)
    layer.effect: Component {
        TerminalCustomShaderPipelineEffect {
            shaderStages: pipelineStages
            uniformProvider: testUniformProvider
        }
    }

    Rectangle {
        width: parent.width / 2
        height: parent.height
        color: "#d02040"
    }
    Rectangle {
        x: parent.width / 2
        width: parent.width / 2
        height: parent.height
        color: "#2040d0"
    }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/custom-shader-retained-resize.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    // Keep the native toplevel size fixed. Some Wayland compositors are free
    // to reject a requested toplevel resize, while the layer item itself must
    // still support deterministic in-place target resizing.
    window.resize(48, 40);
    root->setParentItem(window.contentItem());
    QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no pre-resize frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    const TerminalCustomShaderPipelineSnapshot before =
        pipeline->renderSnapshot();
    QVERIFY2(before.diagnostic.isEmpty(), qPrintable(before.diagnostic));
    QCOMPARE(before.liveTargetCount, 2);
    QCOMPARE(before.targetPixelSize, QSize(32, 24));
    QVERIFY(before.pipelineCreateCount >= 3);

    root->setSize(QSizeF(48.0, 40.0));
    window.update();
    frame = window.grabWindow();
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no post-resize frame.");
    QCOMPARE(frame.size(), QSize(48, 40));

    const QColor left = frame.pixelColor(12, 20);
    const QColor right = frame.pixelColor(36, 20);
    QVERIFY2(isMostly(left, QColor(QStringLiteral("#d02040"))),
             qPrintable(QStringLiteral("unexpected resized left sample %1")
                            .arg(colorDescription(left))));
    QVERIFY2(isMostly(right, QColor(QStringLiteral("#2040d0"))),
             qPrintable(QStringLiteral("unexpected resized right sample %1")
                            .arg(colorDescription(right))));

    const TerminalCustomShaderPipelineSnapshot after =
        pipeline->renderSnapshot();
    QVERIFY2(after.diagnostic.isEmpty(), qPrintable(after.diagnostic));
    QCOMPARE(after.liveTargetCount, before.liveTargetCount);
    QCOMPARE(after.targetPixelSize, QSize(48, 40));
    QCOMPARE(after.ownedTextureBytes,
             static_cast<std::uint64_t>(2 * 48 * 40 * 4));
    QCOMPARE(after.targetCreateCount, before.targetCreateCount);
    QCOMPARE(after.targetDestroyCount, before.targetDestroyCount);
    QVERIFY(after.resourceGeneration > before.resourceGeneration);
    QCOMPARE(after.pipelineCreateCount, before.pipelineCreateCount);
    QCOMPARE(after.liveBindingCount, before.liveBindingCount);
    QCOMPARE(after.bindingCreateCount, before.bindingCreateCount);

    root.reset();
}

void TerminalCustomShaderRhiTest::
    retainedParentOpacityCompositesPremultipliedContent()
{
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList(
            {identityStage_, identityStage_}));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 32

    Rectangle {
        anchors.fill: parent
        color: "#204060"
    }

    Item {
        anchors.fill: parent
        opacity: 0.5

        Item {
            x: 4
            y: 4
            width: 24
            height: 24
            layer.enabled: true
            layer.live: true
            layer.smooth: false
            layer.textureMirroring: ShaderEffectSource.NoMirroring
            layer.textureSize: Qt.size(width, height)
            layer.effect: Component {
                TerminalCustomShaderPipelineEffect {
                    shaderStages: pipelineStages
                    uniformProvider: testUniformProvider
                }
            }
            Rectangle {
                anchors.fill: parent
                color: "#80e04020"
            }
        }
    }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/custom-shader-retained-opacity.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    const QColor background(QStringLiteral("#204060"));
    const QColor source(QStringLiteral("#80e04020"));
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    const QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no opacity frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    const QColor outside = frame.pixelColor(1, 1);
    const QColor inside = frame.pixelColor(16, 16);
    const QColor expected = compositeColor(source, 0.5, background);
    QVERIFY2(isMostly(outside, background),
             qPrintable(QStringLiteral("unexpected background sample %1")
                            .arg(colorDescription(outside))));
    QVERIFY2(
        isMostly(inside, expected),
        qPrintable(
            QStringLiteral("unexpected premultiplied opacity sample %1; "
                           "expected %2")
                .arg(colorDescription(inside), colorDescription(expected))));
    QVERIFY(near(inside.alpha(), expected.alpha()));

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    QCOMPARE(pipeline->renderSnapshot().uniformSlotCount, 2);
    QVERIFY2(pipeline->renderDiagnostic().isEmpty(),
             qPrintable(pipeline->renderDiagnostic()));

    root.reset();
}

void TerminalCustomShaderRhiTest::retainedRectangularClipUsesScissor()
{
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList({identityStage_}));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 32
    height: 32

    Rectangle {
        anchors.fill: parent
        color: "#203040"
    }

    Item {
        x: 8
        y: 6
        width: 16
        height: 20
        clip: true

        Item {
            x: -8
            y: -6
            width: 32
            height: 32
            layer.enabled: true
            layer.live: true
            layer.smooth: false
            layer.textureMirroring: ShaderEffectSource.NoMirroring
            layer.textureSize: Qt.size(width, height)
            layer.effect: Component {
                TerminalCustomShaderPipelineEffect {
                    shaderStages: pipelineStages
                    uniformProvider: testUniformProvider
                }
            }
            Rectangle {
                anchors.fill: parent
                color: "#20d060"
            }
        }
    }
}
)qml",
        QUrl(QStringLiteral("qrc:/test/custom-shader-retained-clip.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(32, 32);
    root->setParentItem(window.contentItem());
    const QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no clipped frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    const QColor background(QStringLiteral("#203040"));
    const QColor foreground(QStringLiteral("#20d060"));
    const std::array<QPoint, 4> outsideSamples{{
        {4, 16},
        {28, 16},
        {16, 3},
        {16, 29},
    }};
    QVERIFY2(isMostly(frame.pixelColor(16, 16), foreground),
             qPrintable(QStringLiteral("unexpected clipped interior sample %1")
                            .arg(colorDescription(frame.pixelColor(16, 16)))));
    for (const QPoint &point : outsideSamples) {
        const QColor actual = frame.pixelColor(point);
        QVERIFY2(isMostly(actual, background),
                 qPrintable(QStringLiteral("clip leaked at (%1,%2): %3")
                                .arg(point.x())
                                .arg(point.y())
                                .arg(colorDescription(actual))));
    }

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    const TerminalCustomShaderPipelineSnapshot snapshot =
        pipeline->renderSnapshot();
    QVERIFY2(snapshot.diagnostic.isEmpty(), qPrintable(snapshot.diagnostic));
    QCOMPARE(snapshot.pipelineCreateCount, static_cast<std::uint64_t>(2));

    root.reset();
}

void TerminalCustomShaderRhiTest::retainedRotatedRectangularClipUsesStencil()
{
    TestUniformProvider provider(Qt::white);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("pipelineStages"),
        terminalCustomShaderStagesToVariantList({identityStage_}));
    engine.rootContext()->setContextProperty(
        QStringLiteral("testUniformProvider"), &provider);

    QQmlComponent component(&engine);
    component.setData(
        R"qml(
import QtQuick
import GhosttyQtShaderRhiTest 1.0

Item {
    width: 48
    height: 48

    Rectangle {
        anchors.fill: parent
        color: "#203040"
    }

    Item {
        x: 12
        y: 12
        width: 24
        height: 24
        transformOrigin: Item.Center
        rotation: 45
        clip: true

        Item {
            x: -12
            y: -12
            width: 48
            height: 48
            layer.enabled: true
            layer.live: true
            layer.smooth: false
            layer.textureMirroring: ShaderEffectSource.NoMirroring
            layer.textureSize: Qt.size(width, height)
            layer.effect: Component {
                TerminalCustomShaderPipelineEffect {
                    shaderStages: pipelineStages
                    uniformProvider: testUniformProvider
                }
            }
            Rectangle {
                anchors.fill: parent
                color: "#20d060"
            }
        }
    }
}
)qml",
        QUrl(QStringLiteral(
            "qrc:/test/custom-shader-retained-stencil-clip.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(
        qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(48, 48);
    root->setParentItem(window.contentItem());
    const QImage frame = renderWindow(window);
    QVERIFY2(!frame.isNull(),
             "Retained RHI pipeline produced no stencil-clipped frame.");
    if (window.rendererInterface()->graphicsApi() != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }

    const QColor background(QStringLiteral("#203040"));
    const QColor foreground(QStringLiteral("#20d060"));
    const std::array<QPoint, 4> outsideSamples{{
        {24, 4},
        {44, 24},
        {24, 44},
        {4, 24},
    }};
    QVERIFY2(isMostly(frame.pixelColor(24, 24), foreground),
             qPrintable(QStringLiteral("unexpected stencil interior sample %1")
                            .arg(colorDescription(frame.pixelColor(24, 24)))));
    for (const QPoint &point : outsideSamples) {
        const QColor actual = frame.pixelColor(point);
        QVERIFY2(isMostly(actual, background),
                 qPrintable(QStringLiteral("stencil clip leaked at (%1,%2): %3")
                                .arg(point.x())
                                .arg(point.y())
                                .arg(colorDescription(actual))));
    }

    TerminalCustomShaderPipelineEffect *const pipeline = provider.pipeline();
    QVERIFY(pipeline != nullptr);
    const TerminalCustomShaderPipelineSnapshot snapshot =
        pipeline->renderSnapshot();
    QVERIFY2(snapshot.diagnostic.isEmpty(), qPrintable(snapshot.diagnostic));
    QCOMPARE(snapshot.pipelineCreateCount, static_cast<std::uint64_t>(2));

    root.reset();
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariable("GHOSTTY_QT_SHADER_TEST_BACKEND")
        == QStringLiteral("vulkan")) {
        requestedGraphicsApi = QSGRendererInterface::Vulkan;
    }
    QQuickWindow::setGraphicsApi(requestedGraphicsApi);
    QGuiApplication application(argc, argv);
    TerminalCustomShaderRhiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_terminal_custom_shader_rhi.moc"
