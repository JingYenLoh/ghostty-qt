#include "terminal_custom_shader_compiler.h"
#include "terminal_custom_shader_qsg.h"

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

#include <memory>
#include <utility>

namespace {

QSGRendererInterface::GraphicsApi requestedGraphicsApi =
    QSGRendererInterface::OpenGL;

class TestUniformProvider final : public QObject,
                                  public TerminalCustomShaderUniformProvider {
    Q_OBJECT
    Q_INTERFACES(TerminalCustomShaderUniformProvider)

public:
    explicit TestUniformProvider(const QColor &color) { setColor(color); }

    TerminalCustomShaderUniformSnapshot
    terminalCustomShaderUniformSnapshot(int) const override
    {
        return uniforms_;
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

    void setColor(const QColor &color)
    {
        auto uniforms = std::make_shared<TerminalCustomShaderUniforms>();
        uniforms->resolution = {32.0F, 32.0F, 1.0F};
        uniforms->backgroundColor = {color.redF(), color.greenF(),
                                     color.blueF(), color.alphaF()};
        uniforms_ = std::move(uniforms);
        for (TerminalCustomShaderEffect *const effect :
             std::as_const(effects_)) {
            if (effect != nullptr) effect->update();
        }
    }

private:
    TerminalCustomShaderUniformSnapshot uniforms_;
    QVector<QPointer<TerminalCustomShaderEffect>> effects_;
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

} // namespace

class TerminalCustomShaderRhiTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void identityShaderPreservesVerticalOrientation();
    void sharedProgramKeepsPerMaterialUniformsIsolated();

private:
    std::unique_ptr<QTemporaryDir> shaderRoot_;
    TerminalCustomShaderStage identityStage_;
    TerminalCustomShaderStage uniformStage_;
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
