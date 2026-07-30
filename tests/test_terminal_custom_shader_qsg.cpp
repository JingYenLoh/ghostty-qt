#include "terminal_custom_shader_compiler.h"
#include "terminal_custom_shader_pipeline.h"
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

    void terminalCustomShaderPipelineAttached(
        TerminalCustomShaderPipelineEffect *effect) override
    {
        pipelineAttachments.append(effect);
    }

    void terminalCustomShaderPipelineDetached(
        TerminalCustomShaderPipelineEffect *effect) override
    {
        pipelineDetachments.append(effect);
    }

    TerminalCustomShaderUniformSnapshot uniforms =
        std::make_shared<const TerminalCustomShaderUniforms>();
    mutable QVector<int> requestedStages;
    QVector<Attachment> attachments;
    QVector<Attachment> detachments;
    QVector<TerminalCustomShaderPipelineEffect *> pipelineAttachments;
    QVector<TerminalCustomShaderPipelineEffect *> pipelineDetachments;
};

class TerminalCustomShaderQsgTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void programRequiresValidSerializedFragmentShader();
    void effectTracksActiveInputsAndProviderStage();
    void destroyedInputsDeactivateEffectSafely();
    void nodeRejectsIncompleteRenderState();
    void pipelineTargetCountIsBounded();
    void pipelineEffectTracksStagesAndProviderAttachment();
    void pipelineTelemetryStartsEmptyAndReportsInvalidStages();
    void destroyedPipelineInputsDeactivateEffectSafely();

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

void TerminalCustomShaderQsgTest::pipelineTargetCountIsBounded()
{
    QCOMPARE(terminalCustomShaderPipelineTargetCount(1), 0);
    QCOMPARE(terminalCustomShaderPipelineTargetCount(2), 1);
    QCOMPARE(terminalCustomShaderPipelineTargetCount(4), 2);
    QCOMPARE(terminalCustomShaderPipelineTargetCount(8), 2);
}

void TerminalCustomShaderQsgTest::
    pipelineEffectTracksStagesAndProviderAttachment()
{
    FakeShaderUniformProvider provider;
    TerminalCustomShaderPipelineEffect *destroyedEffect = nullptr;

    {
        TerminalCustomShaderPipelineEffect effect;
        destroyedEffect = &effect;
        QQuickItem source;
        QSignalSpy activeChanged(
            &effect, &TerminalCustomShaderPipelineEffect::activeChanged);
        QSignalSpy stagesChanged(
            &effect, &TerminalCustomShaderPipelineEffect::shaderStagesChanged);

        QVERIFY(!effect.isActive());
        effect.setUniformProvider(&provider);
        QCOMPARE(provider.pipelineAttachments,
                 (QVector<TerminalCustomShaderPipelineEffect *>{&effect}));
        QVERIFY(provider.pipelineDetachments.isEmpty());
        QVERIFY(!effect.isActive());

        effect.setSource(&source);
        QVERIFY(!effect.isActive());
        effect.setStages({validStage_});
        QVERIFY(effect.isActive());
        QCOMPARE(effect.stages(),
                 (QVector<TerminalCustomShaderStage>{validStage_}));
        QCOMPARE(effect.shaderStages(),
                 terminalCustomShaderStagesToVariantList({validStage_}));
        QCOMPARE(activeChanged.count(), 1);
        QCOMPARE(stagesChanged.count(), 1);
        QCOMPARE(effect.renderSnapshot().passCount, 1);

        effect.setStages({validStage_, validStage_});
        QVERIFY(effect.isActive());
        QCOMPARE(activeChanged.count(), 1);
        QCOMPARE(stagesChanged.count(), 2);
        QCOMPARE(effect.renderSnapshot().passCount, 2);

        effect.setStages({validStage_, validStage_});
        QCOMPARE(stagesChanged.count(), 2);

        effect.setUniformProvider(nullptr);
        QVERIFY(!effect.isActive());
        QCOMPARE(activeChanged.count(), 2);
        QCOMPARE(provider.pipelineDetachments,
                 (QVector<TerminalCustomShaderPipelineEffect *>{&effect}));

        effect.setUniformProvider(&provider);
        QVERIFY(effect.isActive());
        QCOMPARE(activeChanged.count(), 3);
        QCOMPARE(provider.pipelineAttachments.size(), 2);
        QCOMPARE(provider.pipelineAttachments.constLast(), &effect);
    }

    QCOMPARE(provider.pipelineDetachments.size(), 2);
    QCOMPARE(provider.pipelineDetachments.constLast(), destroyedEffect);
}

void TerminalCustomShaderQsgTest::
    pipelineTelemetryStartsEmptyAndReportsInvalidStages()
{
    TerminalCustomShaderPipelineEffect effect;
    const TerminalCustomShaderPipelineSnapshot initial =
        effect.renderSnapshot();
    QVERIFY(initial == TerminalCustomShaderPipelineSnapshot{});
    QVERIFY(effect.renderDiagnostic().isEmpty());

    QSignalSpy stagesChanged(
        &effect, &TerminalCustomShaderPipelineEffect::shaderStagesChanged);
    effect.setShaderStages({QStringLiteral("not a stage map")});

    QVERIFY(!effect.isActive());
    QVERIFY(effect.stages().isEmpty());
    QVERIFY(effect.shaderStages().isEmpty());
    QCOMPARE(stagesChanged.count(), 1);
    QCOMPARE(effect.renderSnapshot().passCount, 0);
    QVERIFY(effect.renderDiagnostic().contains(QStringLiteral("not a map")));

    QVariantMap incompleteStage;
    incompleteStage.insert(QStringLiteral("qsbPath"), validStage_.qsbPath);
    effect.setShaderStages({incompleteStage});
    QVERIFY(!effect.isActive());
    QVERIFY(
        effect.renderDiagnostic().contains(QStringLiteral("is incomplete")));

    QQuickItem source;
    FakeShaderUniformProvider provider;
    effect.setSource(&source);
    effect.setUniformProvider(&provider);
    QVariantMap malformedStage;
    malformedStage.insert(QStringLiteral("sourcePath"), validStage_.sourcePath);
    malformedStage.insert(QStringLiteral("qsbPath"), validStage_.qsbPath);
    malformedStage.insert(QStringLiteral("cacheKey"), validStage_.cacheKey);
    malformedStage.insert(QStringLiteral("serializedShader"),
                          QByteArrayLiteral("not a serialized QShader"));
    effect.setShaderStages({malformedStage});
    QVERIFY(!effect.isActive());
    QCOMPARE(effect.renderSnapshot().passCount, 1);

    effect.setShaderStages(
        terminalCustomShaderStagesToVariantList({validStage_}));
    QVERIFY(effect.isActive());
    QVERIFY(effect.renderDiagnostic().isEmpty());
}

void TerminalCustomShaderQsgTest::
    destroyedPipelineInputsDeactivateEffectSafely()
{
    TerminalCustomShaderPipelineEffect effect;
    auto source = std::make_unique<QQuickItem>();
    auto provider = std::make_unique<FakeShaderUniformProvider>();
    effect.setStages({validStage_});
    effect.setSource(source.get());
    effect.setUniformProvider(provider.get());
    QVERIFY(effect.isActive());

    QSignalSpy sourceChanged(
        &effect, &TerminalCustomShaderPipelineEffect::sourceChanged);
    QSignalSpy providerChanged(
        &effect, &TerminalCustomShaderPipelineEffect::uniformProviderChanged);
    QSignalSpy activeChanged(
        &effect, &TerminalCustomShaderPipelineEffect::activeChanged);

    source.reset();
    QVERIFY(!effect.isActive());
    QCOMPARE(effect.source(), nullptr);
    QCOMPARE(sourceChanged.count(), 1);
    QCOMPARE(activeChanged.count(), 1);

    source = std::make_unique<QQuickItem>();
    effect.setSource(source.get());
    QVERIFY(effect.isActive());
    QCOMPARE(sourceChanged.count(), 2);
    QCOMPARE(activeChanged.count(), 2);

    provider.reset();
    QVERIFY(!effect.isActive());
    QCOMPARE(effect.uniformProvider(), nullptr);
    QCOMPARE(providerChanged.count(), 1);
    QCOMPARE(activeChanged.count(), 3);
}

QTEST_MAIN(TerminalCustomShaderQsgTest)

#include "test_terminal_custom_shader_qsg.moc"
