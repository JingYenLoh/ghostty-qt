#include "terminal_custom_shader_compiler.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <QtShaderTools/rhi/qshaderbaker.h>
#include <rhi/qshader.h>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <ranges>
#include <sys/stat.h>
#include <unistd.h>

namespace {

QString writeShader(const QDir &directory, QStringView name,
                    QByteArrayView source)
{
    const QString path = directory.filePath(name.toString());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(source.data(), source.size()) != source.size()) {
        return {};
    }
    return path;
}

TerminalCustomShaderOptions
optionsFor(std::initializer_list<GhosttyConfigPath> sources)
{
    TerminalCustomShaderOptions options;
    options.sources = QVector<GhosttyConfigPath>(sources);
    return options;
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

int openFifoWriterWhenReaderReady(const QString &path,
                                  std::chrono::milliseconds timeout)
{
    const QByteArray encoded = QFile::encodeName(path);
    QDeadlineTimer deadline(timeout);
    while (!deadline.hasExpired()) {
        const int descriptor =
            ::open(encoded.constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (descriptor >= 0) {
            return descriptor;
        }
        if (errno != ENXIO && errno != EINTR) {
            return -1;
        }
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return -1;
}

bool writeAndCloseFifo(int descriptor, QByteArrayView source)
{
    const ssize_t written =
        ::write(descriptor, source.data(), static_cast<size_t>(source.size()));
    return ::close(descriptor) == 0
        && written == static_cast<ssize_t>(source.size());
}

} // namespace

class TerminalCustomShaderCompilerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void emptyConfigurationAvoidsCacheSetup();
    void compilesAndReusesContentAddressedCache();
    void recoversFromCorruptedContentCache();
    void recoversFromStructurallyIncompatibleContentCache();
    void preservesOrderAndSkipsOnlyMissingOptionalFiles();
    void presentOptionalSourceMustCompile();
    void rejectsWholeChainWhenRequiredSourceFails();
    void rejectsOversizedSourceBeforeBake();
    void rejectsInvalidShader();
    void supportsGhosttyCompatibilityPreambleAndRuntimeTargets();
    void compilesBouncingDvdExample();
    void compilesFlameLifecycleExample();
    void compilesLifecycleExample();
    void exportsStableGhosttyUniformLayout();
    void coalescesConcurrentRequests();
    void rerunsRequestQueuedDuringLiveEdit();
    void dropsCompletionWhenContextDies();
};

void TerminalCustomShaderCompilerTest::emptyConfigurationAvoidsCacheSetup()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString cache =
        QDir(temporary.path()).filePath(QStringLiteral("not-created"));
    QVERIFY(!QFileInfo::exists(cache));

    const TerminalCustomShaderCompileResult result =
        compileTerminalCustomShaders({}, cache);

    QVERIFY2(result.succeeded(), qPrintable(result.diagnostic));
    QVERIFY(result.stages.isEmpty());
    QCOMPARE(result.metrics.sourceCount, 0);
    QCOMPARE(result.metrics.compiledCount, 0);
    QCOMPARE(result.metrics.cacheHitCount, 0);
    QVERIFY(!QFileInfo::exists(cache));
}

void TerminalCustomShaderCompilerTest::compilesAndReusesContentAddressedCache()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString shaderPath =
        writeShader(directory, u"identity.glsl", QByteArrayView(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = texture(iChannel0, fragCoord / iResolution.xy);
}
)glsl"));
    QVERIFY(!shaderPath.isEmpty());

    const TerminalCustomShaderOptions options =
        optionsFor({{.path = shaderPath, .optional = false}});
    const QString cache = directory.filePath(QStringLiteral("cache"));
    const TerminalCustomShaderCompileResult cold =
        compileTerminalCustomShaders(options, cache);
    QVERIFY2(cold.succeeded(), qPrintable(cold.diagnostic));
    QCOMPARE(cold.stages.size(), 1);
    QCOMPARE(cold.metrics.compiledCount, 1);
    QCOMPARE(cold.metrics.cacheHitCount, 0);
    QVERIFY(QFileInfo::exists(cold.stages.constFirst().qsbPath));

    const TerminalCustomShaderCompileResult warm =
        compileTerminalCustomShaders(options, cache);
    QVERIFY2(warm.succeeded(), qPrintable(warm.diagnostic));
    QCOMPARE(warm.stages, cold.stages);
    QCOMPARE(warm.metrics.compiledCount, 0);
    QCOMPARE(warm.metrics.cacheHitCount, 1);

    QFile source(shaderPath);
    QVERIFY(source.open(QIODevice::Append));
    QCOMPARE(source.write("\n// content change\n"), 19);
    source.close();
    const TerminalCustomShaderCompileResult changed =
        compileTerminalCustomShaders(options, cache);
    QVERIFY2(changed.succeeded(), qPrintable(changed.diagnostic));
    QCOMPARE(changed.metrics.compiledCount, 1);
    QCOMPARE(changed.metrics.cacheHitCount, 0);
    QVERIFY(changed.stages.constFirst().cacheKey
            != cold.stages.constFirst().cacheKey);
}

void TerminalCustomShaderCompilerTest::recoversFromCorruptedContentCache()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString shaderPath =
        writeShader(directory, u"identity.glsl", QByteArrayView(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = texture(iChannel0, fragCoord / iResolution.xy);
}
)glsl"));
    QVERIFY(!shaderPath.isEmpty());

    const TerminalCustomShaderOptions options =
        optionsFor({{.path = shaderPath, .optional = false}});
    const QString cache = directory.filePath(QStringLiteral("cache"));
    const TerminalCustomShaderCompileResult first =
        compileTerminalCustomShaders(options, cache);
    QVERIFY2(first.succeeded(), qPrintable(first.diagnostic));
    QCOMPARE(first.metrics.compiledCount, 1);

    QFile corrupted(first.stages.constFirst().qsbPath);
    QVERIFY(corrupted.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupted.write("not a serialized shader"), 23);
    corrupted.close();

    const TerminalCustomShaderCompileResult recovered =
        compileTerminalCustomShaders(options, cache);
    QVERIFY2(recovered.succeeded(), qPrintable(recovered.diagnostic));
    QCOMPARE(recovered.metrics.compiledCount, 1);
    QCOMPARE(recovered.metrics.cacheHitCount, 0);
    QCOMPARE(recovered.stages.constFirst().cacheKey,
             first.stages.constFirst().cacheKey);

    QFile rebuilt(recovered.stages.constFirst().qsbPath);
    QVERIFY(rebuilt.open(QIODevice::ReadOnly));
    QVERIFY(QShader::fromSerialized(rebuilt.readAll()).isValid());
}

void TerminalCustomShaderCompilerTest::
    recoversFromStructurallyIncompatibleContentCache()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString shaderPath =
        writeShader(directory, u"identity.glsl", QByteArrayView(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = texture(iChannel0, fragCoord / iResolution.xy);
}
)glsl"));
    QVERIFY(!shaderPath.isEmpty());

    const TerminalCustomShaderOptions options =
        optionsFor({{.path = shaderPath, .optional = false}});
    const QString cache = directory.filePath(QStringLiteral("cache"));
    const TerminalCustomShaderCompileResult first =
        compileTerminalCustomShaders(options, cache);
    QVERIFY2(first.succeeded(), qPrintable(first.diagnostic));
    QCOMPARE(first.metrics.compiledCount, 1);

    QShaderBaker baker;
    baker.setSourceString(QByteArrayLiteral(R"glsl(
#version 440
layout(location = 0) out vec4 color;
void main()
{
    color = vec4(1.0);
}
)glsl"),
                          QShader::FragmentStage);
    baker.setGeneratedShaders({
        {QShader::SpirvShader, QShaderVersion(100)},
        {QShader::GlslShader, QShaderVersion(330)},
        {QShader::GlslShader, QShaderVersion(430)},
        {QShader::GlslShader, QShaderVersion(300, QShaderVersion::GlslEs)},
    });
    baker.setGeneratedShaderVariants({QShader::StandardShader});
    const QShader incompatible = baker.bake();
    QVERIFY2(incompatible.isValid(), qPrintable(baker.errorMessage()));
    QCOMPARE(incompatible.stage(), QShader::FragmentStage);

    QFile cached(first.stages.constFirst().qsbPath);
    QVERIFY(cached.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray incompatibleBytes = incompatible.serialized();
    QCOMPARE(cached.write(incompatibleBytes), incompatibleBytes.size());
    cached.close();

    const TerminalCustomShaderCompileResult recovered =
        compileTerminalCustomShaders(options, cache);
    QVERIFY2(recovered.succeeded(), qPrintable(recovered.diagnostic));
    QCOMPARE(recovered.metrics.compiledCount, 1);
    QCOMPARE(recovered.metrics.cacheHitCount, 0);
    QCOMPARE(recovered.stages.constFirst(), first.stages.constFirst());
}

void TerminalCustomShaderCompilerTest::
    preservesOrderAndSkipsOnlyMissingOptionalFiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QByteArray pass = R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = texture(iChannel0, fragCoord / iResolution.xy);
}
)glsl";
    const QString first = writeShader(directory, u"first.glsl", pass);
    const QString second = writeShader(directory, u"second.glsl", pass);
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    const QString missing = directory.filePath(QStringLiteral("missing.glsl"));

    const TerminalCustomShaderCompileResult result =
        compileTerminalCustomShaders(
            optionsFor({
                {.path = first, .optional = false},
                {.path = missing, .optional = true},
                {.path = second, .optional = false},
            }),
            directory.filePath(QStringLiteral("cache")));
    QVERIFY2(result.succeeded(), qPrintable(result.diagnostic));
    QCOMPARE(result.stages.size(), 2);
    QCOMPARE(result.stages.at(0).sourcePath, first);
    QCOMPARE(result.stages.at(1).sourcePath, second);
}

void TerminalCustomShaderCompilerTest::presentOptionalSourceMustCompile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString invalid =
        writeShader(directory, u"optional-but-invalid.glsl",
                    QByteArrayView("this is not valid GLSL\n"));
    QVERIFY(!invalid.isEmpty());

    const TerminalCustomShaderCompileResult result =
        compileTerminalCustomShaders(
            optionsFor({{.path = invalid, .optional = true}}),
            directory.filePath(QStringLiteral("cache")));

    QVERIFY(!result.succeeded());
    QVERIFY(result.stages.isEmpty());
    QVERIFY(result.diagnostic.contains(
        QStringLiteral("optional-but-invalid.glsl")));
}

void TerminalCustomShaderCompilerTest::
    rejectsWholeChainWhenRequiredSourceFails()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString valid =
        writeShader(directory, u"valid.glsl", QByteArrayView(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = vec4(fragCoord / iResolution.xy, 0.0, 1.0);
}
)glsl"));
    QVERIFY(!valid.isEmpty());

    const TerminalCustomShaderCompileResult result =
        compileTerminalCustomShaders(
            optionsFor({
                {.path = valid, .optional = false},
                {.path = directory.filePath(QStringLiteral("required.glsl")),
                 .optional = false},
            }),
            directory.filePath(QStringLiteral("cache")));
    QVERIFY(!result.succeeded());
    QVERIFY(result.stages.isEmpty());
    QVERIFY(result.diagnostic.contains(QStringLiteral("required.glsl")));
}

void TerminalCustomShaderCompilerTest::rejectsOversizedSourceBeforeBake()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString shaderPath =
        directory.filePath(QStringLiteral("oversized.glsl"));
    QFile shader(shaderPath);
    QVERIFY(shader.open(QIODevice::WriteOnly));
    constexpr qsizetype limit = 4 * 1024 * 1024;
    const QByteArray oversized(limit + 1, ' ');
    QCOMPARE(shader.write(oversized), oversized.size());
    shader.close();

    const TerminalCustomShaderCompileResult result =
        compileTerminalCustomShaders(
            optionsFor({{.path = shaderPath, .optional = false}}),
            directory.filePath(QStringLiteral("cache")));

    QVERIFY(!result.succeeded());
    QVERIFY(result.stages.isEmpty());
    QCOMPARE(result.metrics.compiledCount, 0);
    QVERIFY(result.diagnostic.contains(QStringLiteral("4 MiB")));
}

void TerminalCustomShaderCompilerTest::rejectsInvalidShader()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString invalid =
        writeShader(directory, u"invalid.glsl",
                    QByteArrayView("vec4 definitelyNotMainImage;\n"));
    QVERIFY(!invalid.isEmpty());

    const TerminalCustomShaderCompileResult result =
        compileTerminalCustomShaders(
            optionsFor({{.path = invalid, .optional = false}}),
            directory.filePath(QStringLiteral("cache")));
    QVERIFY(!result.succeeded());
    QVERIFY(result.stages.isEmpty());
    QVERIFY(result.diagnostic.contains(QStringLiteral("invalid.glsl")));
}

void TerminalCustomShaderCompilerTest::
    supportsGhosttyCompatibilityPreambleAndRuntimeTargets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString shaderPath =
        writeShader(directory, u"compatibility.glsl", QByteArrayView(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    int cursorStyles = CURSORSTYLE_BLOCK + CURSORSTYLE_BLOCK_HOLLOW
        + CURSORSTYLE_BAR + CURSORSTYLE_UNDERLINE + CURSORSTYLE_LOCK;
    vec2 uv = fragCoord / iResolution.xy;
    color = texture2D(iChannel0, uv) + vec4(float(cursorStyles) * 0.0);
}
)glsl"));
    QVERIFY(!shaderPath.isEmpty());

    const TerminalCustomShaderCompileResult result =
        compileTerminalCustomShaders(
            optionsFor({{.path = shaderPath, .optional = false}}),
            directory.filePath(QStringLiteral("cache")));
    QVERIFY2(result.succeeded(), qPrintable(result.diagnostic));

    QFile qsb(result.stages.constFirst().qsbPath);
    QVERIFY(qsb.open(QIODevice::ReadOnly));
    const QShader shader = QShader::fromSerialized(qsb.readAll());
    QVERIFY(shader.isValid());

    bool hasSpirv100 = false;
    bool hasGlsl330 = false;
    bool hasGlsl430 = false;
    bool hasGlslEs300 = false;
    for (const QShaderKey &key : shader.availableShaders()) {
        if (key.source() == QShader::SpirvShader
            && key.sourceVersion().version() == 100) {
            hasSpirv100 = true;
        } else if (key.source() == QShader::GlslShader
                   && key.sourceVersion().version() == 330
                   && !key.sourceVersion().flags().testFlag(
                       QShaderVersion::GlslEs)) {
            hasGlsl330 = true;
        } else if (key.source() == QShader::GlslShader
                   && key.sourceVersion().version() == 430
                   && !key.sourceVersion().flags().testFlag(
                       QShaderVersion::GlslEs)) {
            hasGlsl430 = true;
        } else if (key.source() == QShader::GlslShader
                   && key.sourceVersion().version() == 300
                   && key.sourceVersion().flags().testFlag(
                       QShaderVersion::GlslEs)) {
            hasGlslEs300 = true;
        }
    }
    QVERIFY(hasSpirv100);
    QVERIFY(hasGlsl330);
    QVERIFY(hasGlsl430);
    QVERIFY(hasGlslEs300);

    const auto samplers = shader.description().combinedImageSamplers();
    QCOMPARE(samplers.size(), 1);
    QCOMPARE(samplers.constFirst().name, QByteArray("iChannel0"));
    QCOMPARE(samplers.constFirst().binding, 1);
}

void TerminalCustomShaderCompilerTest::exportsStableGhosttyUniformLayout()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString shaderPath =
        writeShader(directory, u"uniforms.glsl", QByteArrayView(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = vec4(iPalette[7] + iBackgroundColor * 0.0, 1.0)
        + vec4(iCurrentCursor.xy / iResolution.xy, 0.0, 0.0)
        + iPaneTransition * 0.0;
}

)glsl"));
    const TerminalCustomShaderCompileResult result =
        compileTerminalCustomShaders(
            optionsFor({{.path = shaderPath, .optional = false}}),
            directory.filePath(QStringLiteral("cache")));
    QVERIFY2(result.succeeded(), qPrintable(result.diagnostic));

    QFile qsb(result.stages.constFirst().qsbPath);
    QVERIFY(qsb.open(QIODevice::ReadOnly));
    const QShader shader = QShader::fromSerialized(qsb.readAll());
    QVERIFY(shader.isValid());
    const QList<QShaderDescription::UniformBlock> blocks =
        shader.description().uniformBlocks();
    QCOMPARE(blocks.size(), 1);
    const auto &block = blocks.constFirst();
    QCOMPARE(block.binding, 0);
    QCOMPARE(block.size, 4'592);

    struct ExpectedMember {
        QByteArrayView name;
        int offset;
    };
    constexpr std::array expectedMembers{
        ExpectedMember{"qt_Matrix", 0},
        ExpectedMember{"qt_Opacity", 64},
        ExpectedMember{"iResolution", 80},
        ExpectedMember{"iTime", 92},
        ExpectedMember{"iTimeDelta", 96},
        ExpectedMember{"iFrameRate", 100},
        ExpectedMember{"iFrame", 104},
        ExpectedMember{"iChannelTime", 112},
        ExpectedMember{"iChannelResolution", 176},
        ExpectedMember{"iMouse", 240},
        ExpectedMember{"iDate", 256},
        ExpectedMember{"iSampleRate", 272},
        ExpectedMember{"iCurrentCursor", 288},
        ExpectedMember{"iPreviousCursor", 304},
        ExpectedMember{"iCurrentCursorColor", 320},
        ExpectedMember{"iPreviousCursorColor", 336},
        ExpectedMember{"iCurrentCursorStyle", 352},
        ExpectedMember{"iPreviousCursorStyle", 356},
        ExpectedMember{"iCursorVisible", 360},
        ExpectedMember{"iTimeCursorChange", 364},
        ExpectedMember{"iTimeFocus", 368},
        ExpectedMember{"iFocus", 372},
        ExpectedMember{"iPalette", 384},
        ExpectedMember{"iBackgroundColor", 4'480},
        ExpectedMember{"iForegroundColor", 4'496},
        ExpectedMember{"iCursorColor", 4'512},
        ExpectedMember{"iCursorText", 4'528},
        ExpectedMember{"iSelectionForegroundColor", 4'544},
        ExpectedMember{"iSelectionBackgroundColor", 4'560},
        ExpectedMember{"_ghosttyQtPadding", 4'572},
        ExpectedMember{"iPaneTransition", 4'576},
    };
    for (const ExpectedMember &expected : expectedMembers) {
        const auto *const member = memberNamed(block, expected.name);
        QVERIFY2(member != nullptr, expected.name.data());
        QVERIFY2(member->offset == expected.offset,
                 qPrintable(QStringLiteral("%1 offset is %2, expected %3")
                                .arg(QString::fromLatin1(expected.name))
                                .arg(member->offset)
                                .arg(expected.offset)));
    }
}

void TerminalCustomShaderCompilerTest::compilesLifecycleExample()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString shaderPath =
        QString::fromUtf8(GHOSTTY_QT_EXAMPLE_SHADER_PATH);
    QVERIFY2(QFileInfo(shaderPath).isFile(), qPrintable(shaderPath));

    const TerminalCustomShaderOptions options =
        optionsFor({{.path = shaderPath, .optional = false}});
    const QString cache =
        QDir(temporary.path()).filePath(QStringLiteral("cache"));
    const TerminalCustomShaderCompileResult persistent =
        compileTerminalCustomShaders(options, cache);
    const TerminalCustomShaderCompileResult enter =
        compileTerminalCustomShaders(
            options, cache, TerminalCustomShaderCompileMode::PaneEnter);
    const TerminalCustomShaderCompileResult exit = compileTerminalCustomShaders(
        options, cache, TerminalCustomShaderCompileMode::PaneExit);
    QVERIFY2(persistent.succeeded(), qPrintable(persistent.diagnostic));
    QVERIFY2(enter.succeeded(), qPrintable(enter.diagnostic));
    QVERIFY2(exit.succeeded(), qPrintable(exit.diagnostic));
    QCOMPARE(persistent.stages.size(), 1);
    QCOMPARE(enter.stages.size(), 1);
    QCOMPARE(exit.stages.size(), 1);
    QVERIFY(persistent.stages.constFirst().cacheKey
            != enter.stages.constFirst().cacheKey);
    QVERIFY(enter.stages.constFirst().cacheKey
            != exit.stages.constFirst().cacheKey);
}

void TerminalCustomShaderCompilerTest::compilesBouncingDvdExample()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString shaderPath = QString::fromUtf8(GHOSTTY_QT_DVD_SHADER_PATH);
    QVERIFY2(QFileInfo(shaderPath).isFile(), qPrintable(shaderPath));

    const TerminalCustomShaderCompileResult result =
        compileTerminalCustomShaders(
            optionsFor({{.path = shaderPath, .optional = false}}),
            QDir(temporary.path()).filePath(QStringLiteral("cache")));
    QVERIFY2(result.succeeded(), qPrintable(result.diagnostic));
    QCOMPARE(result.stages.size(), 1);
}

void TerminalCustomShaderCompilerTest::compilesFlameLifecycleExample()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString shaderPath = QString::fromUtf8(GHOSTTY_QT_FLAME_SHADER_PATH);
    QVERIFY2(QFileInfo(shaderPath).isFile(), qPrintable(shaderPath));

    const TerminalCustomShaderOptions options =
        optionsFor({{.path = shaderPath, .optional = false}});
    const QString cache =
        QDir(temporary.path()).filePath(QStringLiteral("cache"));
    const TerminalCustomShaderCompileResult enter =
        compileTerminalCustomShaders(
            options, cache, TerminalCustomShaderCompileMode::PaneEnter);
    const TerminalCustomShaderCompileResult exit = compileTerminalCustomShaders(
        options, cache, TerminalCustomShaderCompileMode::PaneExit);
    QVERIFY2(enter.succeeded(), qPrintable(enter.diagnostic));
    QVERIFY2(exit.succeeded(), qPrintable(exit.diagnostic));
    QCOMPARE(enter.stages.size(), 1);
    QCOMPARE(exit.stages.size(), 1);
}

void TerminalCustomShaderCompilerTest::coalescesConcurrentRequests()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString shaderPath =
        writeShader(directory, u"coalesced.glsl", QByteArrayView(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = texture(iChannel0, fragCoord / iResolution.xy);
}
)glsl"));
    QVERIFY(!shaderPath.isEmpty());

    TerminalCustomShaderCompileBroker broker;
    const TerminalCustomShaderOptions options =
        optionsFor({{.path = shaderPath, .optional = false}});
    int callbackCount = 0;
    TerminalCustomShaderCompileResult first;
    TerminalCustomShaderCompileResult second;
    broker.request(
        options, this,
        [&](TerminalCustomShaderCompileResult result) {
            first = std::move(result);
            ++callbackCount;
        },
        directory.filePath(QStringLiteral("broker-cache")));
    broker.request(
        options, this,
        [&](TerminalCustomShaderCompileResult result) {
            second = std::move(result);
            ++callbackCount;
        },
        directory.filePath(QStringLiteral("broker-cache")));
    QCOMPARE(broker.inFlightCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(callbackCount, 2, 10'000);
    QCOMPARE(broker.inFlightCount(), 0);
    QVERIFY2(first.succeeded(), qPrintable(first.diagnostic));
    QVERIFY2(second.succeeded(), qPrintable(second.diagnostic));
    QCOMPARE(first.stages, second.stages);
    QCOMPARE(first.metrics, second.metrics);
}

void TerminalCustomShaderCompilerTest::rerunsRequestQueuedDuringLiveEdit()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    constexpr QByteArrayView firstSource(R"glsl(
void mainImage(out vec4 color, in vec2)
{
    color = vec4(1.0, 0.0, 0.0, 1.0);
}
)glsl");
    constexpr QByteArrayView secondSource(R"glsl(
void mainImage(out vec4 color, in vec2)
{
    color = vec4(0.0, 0.0, 1.0, 1.0);
}
)glsl");
    constexpr QByteArrayView fifoSource(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = vec4(fragCoord / iResolution.xy, 0.0, 1.0);
}
)glsl");
    const QString livePath = writeShader(directory, u"live.glsl", firstSource);
    QVERIFY(!livePath.isEmpty());
    const QString fifoPath =
        directory.filePath(QStringLiteral("compile-barrier.glsl"));
    const QByteArray encodedFifoPath = QFile::encodeName(fifoPath);
    QCOMPARE(::mkfifo(encodedFifoPath.constData(), 0600), 0);

    const TerminalCustomShaderOptions options = optionsFor({
        {.path = livePath, .optional = false},
        {.path = fifoPath, .optional = false},
    });
    const QString cache = directory.filePath(QStringLiteral("broker-cache"));
    TerminalCustomShaderCompileBroker broker;
    int firstCallbackCount = 0;
    int secondCallbackCount = 0;
    TerminalCustomShaderCompileResult first;
    TerminalCustomShaderCompileResult second;
    broker.request(
        options, this,
        [&](TerminalCustomShaderCompileResult result) {
            first = std::move(result);
            ++firstCallbackCount;
        },
        cache);

    // The FIFO is the second source. A writer can open only after the worker
    // has read and baked live.glsl, and keeping it empty holds that first
    // compilation in flight while the same-path edit is queued.
    int writer =
        openFifoWriterWhenReaderReady(fifoPath, std::chrono::seconds(10));
    QVERIFY2(writer >= 0, "compiler did not reach the FIFO barrier");
    const auto closeWriter = qScopeGuard([&writer] {
        if (writer >= 0) {
            (void)::close(writer);
        }
    });
    QCOMPARE(writeShader(directory, u"live.glsl", secondSource), livePath);
    broker.request(
        options, this,
        [&](TerminalCustomShaderCompileResult result) {
            second = std::move(result);
            ++secondCallbackCount;
        },
        cache);
    QCOMPARE(broker.inFlightCount(), 1);
    const bool wroteFirstBarrier = writeAndCloseFifo(writer, fifoSource);
    writer = -1;
    QVERIFY(wroteFirstBarrier);

    QTRY_COMPARE_WITH_TIMEOUT(firstCallbackCount, 1, 10'000);
    QCOMPARE(secondCallbackCount, 0);
    QCOMPARE(broker.inFlightCount(), 1);
    writer = openFifoWriterWhenReaderReady(fifoPath, std::chrono::seconds(10));
    QVERIFY2(writer >= 0, "queued compiler rerun did not reach the FIFO");
    const bool wroteSecondBarrier = writeAndCloseFifo(writer, fifoSource);
    writer = -1;
    QVERIFY(wroteSecondBarrier);

    QTRY_COMPARE_WITH_TIMEOUT(secondCallbackCount, 1, 10'000);
    QCOMPARE(broker.inFlightCount(), 0);
    QVERIFY2(first.succeeded(), qPrintable(first.diagnostic));
    QVERIFY2(second.succeeded(), qPrintable(second.diagnostic));
    QCOMPARE(first.stages.size(), 2);
    QCOMPARE(second.stages.size(), 2);
    QVERIFY(first.stages.at(0).cacheKey != second.stages.at(0).cacheKey);
    QVERIFY(first.stages.at(0).serializedShader
            != second.stages.at(0).serializedShader);
    QCOMPARE(first.stages.at(1).cacheKey, second.stages.at(1).cacheKey);
}

void TerminalCustomShaderCompilerTest::dropsCompletionWhenContextDies()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir directory(temporary.path());
    const QString shaderPath =
        writeShader(directory, u"context.glsl", QByteArrayView(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    color = vec4(fragCoord / iResolution.xy, 0.0, 1.0);
}
)glsl"));
    QVERIFY(!shaderPath.isEmpty());

    TerminalCustomShaderCompileBroker broker;
    auto context = std::make_unique<QObject>();
    int callbackCount = 0;
    broker.request(
        optionsFor({{.path = shaderPath, .optional = false}}), context.get(),
        [&](TerminalCustomShaderCompileResult) { ++callbackCount; },
        directory.filePath(QStringLiteral("broker-cache")));
    QCOMPARE(broker.inFlightCount(), 1);
    context.reset();

    QTRY_COMPARE_WITH_TIMEOUT(broker.inFlightCount(), 0, 10'000);
    QCOMPARE(callbackCount, 0);
}

QTEST_GUILESS_MAIN(TerminalCustomShaderCompilerTest)

#include "test_terminal_custom_shader_compiler.moc"
