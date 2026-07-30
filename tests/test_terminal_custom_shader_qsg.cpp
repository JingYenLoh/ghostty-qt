#include "terminal_custom_shader_compiler.h"
#include "terminal_custom_shader_qsg.h"

#include <QDir>
#include <QFile>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

class FakeShaderUniformProvider final
    : public QObject,
      public TerminalCustomShaderUniformProvider {
    Q_OBJECT
    Q_INTERFACES(TerminalCustomShaderUniformProvider)

public:
    struct Attachment {
        TerminalCustomShaderEffect *effect = nullptr;
        int stageIndex = 0;

        bool operator==(const Attachment &) const = default;
    };

    TerminalCustomShaderUniformSnapshot
    terminalCustomShaderUniformSnapshot(int stageIndex) const override
    {
        requestedStages.append(stageIndex);
        return uniforms;
    }

    void terminalCustomShaderEffectAttached(TerminalCustomShaderEffect *effect,
                                            int stageIndex) override
    {
        attachments.append({effect, stageIndex});
    }

    void terminalCustomShaderEffectDetached(TerminalCustomShaderEffect *effect,
                                            int stageIndex) override
    {
        detachments.append({effect, stageIndex});
    }

    TerminalCustomShaderUniformSnapshot uniforms =
        std::make_shared<const TerminalCustomShaderUniforms>();
    mutable QVector<int> requestedStages;
    QVector<Attachment> attachments;
    QVector<Attachment> detachments;
};

class TerminalCustomShaderQsgTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void programRequiresValidSerializedFragmentShader();
    void effectTracksActiveInputsAndProviderStage();
    void destroyedInputsDeactivateEffectSafely();
    void nodeRejectsIncompleteRenderState();

private:
    std::unique_ptr<QTemporaryDir> shaderRoot_;
    TerminalCustomShaderStage validStage_;
};

void TerminalCustomShaderQsgTest::initTestCase()
{
    shaderRoot_ = std::make_unique<QTemporaryDir>();
    QVERIFY(shaderRoot_->isValid());
    const QDir directory(shaderRoot_->path());
    const QString sourcePath =
        directory.filePath(QStringLiteral("identity.glsl"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    constexpr QByteArrayView body(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = texture(iChannel0, fragCoord / iResolution.xy);
}
)glsl");
    QCOMPARE(source.write(body.data(), body.size()), body.size());
    source.close();

    TerminalCustomShaderOptions options;
    options.sources.append({.path = sourcePath, .optional = false});
    const TerminalCustomShaderCompileResult result =
        compileTerminalCustomShaders(
            options, directory.filePath(QStringLiteral("cache")));
    QVERIFY2(result.succeeded(), qPrintable(result.diagnostic));
    QCOMPARE(result.stages.size(), 1);
    validStage_ = result.stages.constFirst();
    QVERIFY(!validStage_.qsbPath.isEmpty());
    QVERIFY(!validStage_.serializedShader.isEmpty());
}

void TerminalCustomShaderQsgTest::programRequiresValidSerializedFragmentShader()
{
    TerminalCustomShaderProgram empty({}, QByteArrayLiteral("unused"), {});
    QVERIFY(!empty.isValid());

    TerminalCustomShaderProgram missingData(validStage_.qsbPath,
                                            validStage_.cacheKey);
    QVERIFY(!missingData.isValid());

    TerminalCustomShaderProgram program(validStage_.qsbPath,
                                        validStage_.cacheKey,
                                        validStage_.serializedShader);
    QVERIFY(program.isValid());
    QCOMPARE(program.qsbPath(), validStage_.qsbPath);
    QCOMPARE(program.cacheKey(), validStage_.cacheKey);
}

void TerminalCustomShaderQsgTest::effectTracksActiveInputsAndProviderStage()
{
    TerminalCustomShaderEffect effect;
    QQuickItem source;
    FakeShaderUniformProvider provider;
    QSignalSpy activeChanged(&effect,
                             &TerminalCustomShaderEffect::activeChanged);
    QSignalSpy shaderChanged(
        &effect, &TerminalCustomShaderEffect::fragmentShaderFileNameChanged);
    QSignalSpy shaderDataChanged(
        &effect, &TerminalCustomShaderEffect::fragmentShaderDataChanged);

    QVERIFY(!effect.isActive());
    effect.setUniformProvider(&provider);
    QCOMPARE(provider.attachments,
             (QVector<FakeShaderUniformProvider::Attachment>{{&effect, 0}}));
    QVERIFY(!effect.isActive());

    effect.setSource(&source);
    QVERIFY(!effect.isActive());
    effect.setStage(validStage_);
    QVERIFY(effect.isActive());
    QCOMPARE(activeChanged.count(), 1);
    QCOMPARE(shaderChanged.count(), 1);
    QCOMPARE(shaderDataChanged.count(), 1);
    QCOMPARE(effect.fragmentShaderData(), validStage_.serializedShader);

    effect.setStageIndex(3);
    QCOMPARE(effect.stageIndex(), 3);
    QCOMPARE(provider.detachments,
             (QVector<FakeShaderUniformProvider::Attachment>{{&effect, 0}}));
    QCOMPARE(provider.attachments.size(), 2);
    QCOMPARE(provider.attachments.constLast(),
             (FakeShaderUniformProvider::Attachment{&effect, 3}));
    QCOMPARE(activeChanged.count(), 1);

    effect.setStage(validStage_);
    QCOMPARE(shaderChanged.count(), 1);
    QCOMPARE(shaderDataChanged.count(), 1);

    effect.setSource(nullptr);
    QVERIFY(!effect.isActive());
    QCOMPARE(activeChanged.count(), 2);
    effect.setSource(&source);
    QVERIFY(effect.isActive());
    QCOMPARE(activeChanged.count(), 3);

    effect.setUniformProvider(nullptr);
    QVERIFY(!effect.isActive());
    QCOMPARE(provider.detachments.size(), 2);
    QCOMPARE(provider.detachments.constLast(),
             (FakeShaderUniformProvider::Attachment{&effect, 3}));
    QCOMPARE(activeChanged.count(), 4);
}

void TerminalCustomShaderQsgTest::destroyedInputsDeactivateEffectSafely()
{
    TerminalCustomShaderEffect effect;
    auto source = std::make_unique<QQuickItem>();
    auto provider = std::make_unique<FakeShaderUniformProvider>();
    effect.setSource(source.get());
    effect.setUniformProvider(provider.get());
    effect.setFragmentShaderFileName(validStage_.qsbPath);
    QVERIFY(!effect.isActive());
    effect.setFragmentShaderData(validStage_.serializedShader);
    QVERIFY(effect.isActive());
    QSignalSpy activeChanged(&effect,
                             &TerminalCustomShaderEffect::activeChanged);

    source.reset();
    QVERIFY(!effect.isActive());
    QCOMPARE(effect.source(), nullptr);
    QCOMPARE(activeChanged.count(), 1);

    source = std::make_unique<QQuickItem>();
    effect.setSource(source.get());
    QVERIFY(effect.isActive());
    QCOMPARE(activeChanged.count(), 2);

    provider.reset();
    QVERIFY(!effect.isActive());
    QCOMPARE(effect.uniformProvider(), nullptr);
    QCOMPARE(activeChanged.count(), 3);
}

void TerminalCustomShaderQsgTest::nodeRejectsIncompleteRenderState()
{
    TerminalCustomShaderQsgNode node;
    QVERIFY(!node.isDrawable());
    const auto program = std::make_shared<const TerminalCustomShaderProgram>(
        validStage_.qsbPath, validStage_.cacheKey,
        validStage_.serializedShader);
    QVERIFY(program->isValid());
    const auto uniforms =
        std::make_shared<const TerminalCustomShaderUniforms>();

    QVERIFY(
        !node.update(nullptr, QRectF(0.0, 0.0, 80.0, 24.0), program, uniforms));
    QVERIFY(!node.isDrawable());
    node.clear();
    QVERIFY(!node.isDrawable());
}

QTEST_MAIN(TerminalCustomShaderQsgTest)

#include "test_terminal_custom_shader_qsg.moc"
