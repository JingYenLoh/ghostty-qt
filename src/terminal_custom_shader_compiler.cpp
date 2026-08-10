#include "terminal_custom_shader_compiler.h"

#include "terminal_custom_shader_qsg.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>

#include <QtShaderTools/rhi/qshaderbaker.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;

constexpr qint64 kMaximumShaderBytes = 4 * 1024 * 1024;
constexpr auto kShaderPrefix = R"glsl(#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 _ghosttyQtFragColor;

layout(std140, binding = 0) uniform GhosttyQtGlobals {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec3 iResolution;
    float iTime;
    float iTimeDelta;
    float iFrameRate;
    int iFrame;
    float iChannelTime[4];
    vec3 iChannelResolution[4];
    vec4 iMouse;
    vec4 iDate;
    float iSampleRate;
    vec4 iCurrentCursor;
    vec4 iPreviousCursor;
    vec4 iCurrentCursorColor;
    vec4 iPreviousCursorColor;
    int iCurrentCursorStyle;
    int iPreviousCursorStyle;
    int iCursorVisible;
    float iTimeCursorChange;
    float iTimeFocus;
    int iFocus;
    vec3 iPalette[256];
    vec3 iBackgroundColor;
    vec3 iForegroundColor;
    vec3 iCursorColor;
    vec3 iCursorText;
    vec3 iSelectionForegroundColor;
    vec3 iSelectionBackgroundColor;
    float _ghosttyQtPadding;
    vec4 iPaneTransition;
};

layout(binding = 1) uniform sampler2D iChannel0;
#define CURSORSTYLE_BLOCK 0
#define CURSORSTYLE_BLOCK_HOLLOW 1
#define CURSORSTYLE_BAR 2
#define CURSORSTYLE_UNDERLINE 3
#define CURSORSTYLE_LOCK 4
#define PANETRANSITION_EXIT -1
#define PANETRANSITION_STABLE 0
#define PANETRANSITION_ENTER 1
#define texture2D texture

)glsl";

constexpr auto kShaderSuffix = R"glsl(

void main()
{
    const vec2 fragCoord = vec2(
        qt_TexCoord0.x * iResolution.x,
        qt_TexCoord0.y * iResolution.y);
    mainImage(_ghosttyQtFragColor, fragCoord);
    _ghosttyQtFragColor *= qt_Opacity;
}
)glsl";

QString defaultCacheDirectory()
{
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(root).filePath(QStringLiteral("custom-shaders"));
}

QString pathForDiagnostic(const GhosttyConfigPath &source)
{
    return QDir::toNativeSeparators(source.path);
}

QByteArray requestKey(const TerminalCustomShaderOptions &options,
                      TerminalCustomShaderCompileMode mode)
{
    QJsonArray sources;
    for (const GhosttyConfigPath &source : options.sources) {
        sources.append(QJsonObject{
            {QStringLiteral("path"), source.path},
            {QStringLiteral("optional"), source.optional},
        });
    }
    QByteArray key = QJsonDocument(sources).toJson(QJsonDocument::Compact);
    key.append('\0');
    key.append(QByteArray::number(static_cast<int>(mode)));
    return key;
}

QByteArray cacheKeyForSource(const QByteArray &source,
                             TerminalCustomShaderCompileMode mode)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(terminalCustomShaderCompilerCacheVersion));
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(QByteArrayView(qVersion()));
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(QByteArrayView("spirv100;glsl330;glsl430;gles300"));
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(QByteArray::number(static_cast<int>(mode)));
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(source);
    return hash.result().toHex();
}

bool ensurePrivateCacheDirectory(const QString &path, QString *diagnostic)
{
    QDir directory;
    if (!directory.mkpath(path)) {
        *diagnostic =
            QStringLiteral("custom-shader: unable to create cache directory %1")
                .arg(QDir::toNativeSeparators(path));
        return false;
    }
    (void)QFile::setPermissions(path,
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner);
    return true;
}

bool hasShaderTarget(const QShader &shader, QShader::Source source,
                     const QShaderVersion &version)
{
    return std::ranges::any_of(
        shader.availableShaders(), [source, &version](const QShaderKey &key) {
            return key.source() == source && key.sourceVersion() == version
                && key.sourceVariant() == QShader::StandardShader;
        });
}

QString shaderContractDiagnostic(const QShader &shader)
{
    if (!shader.isValid() || shader.stage() != QShader::FragmentStage) {
        return QStringLiteral("compiled output is not a fragment shader");
    }
    if (!hasShaderTarget(shader, QShader::SpirvShader, QShaderVersion(100))
        || !hasShaderTarget(shader, QShader::GlslShader, QShaderVersion(330))
        || !hasShaderTarget(shader, QShader::GlslShader, QShaderVersion(430))
        || !hasShaderTarget(shader, QShader::GlslShader,
                            QShaderVersion(300, QShaderVersion::GlslEs))) {
        return QStringLiteral(
            "one or more required SPIR-V 100, desktop GLSL 330/430, or "
            "GLES 300 runtime targets were not generated");
    }

    const QList<QShaderDescription::UniformBlock> blocks =
        shader.description().uniformBlocks();
    if (blocks.size() != 1 || blocks.constFirst().binding != 0
        || blocks.constFirst().size
            != static_cast<int>(TerminalCustomShaderUniformLayout::size)) {
        return QStringLiteral(
            "generated uniform block does not match the Ghostty ABI");
    }

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
    const QList<QShaderDescription::BlockVariable> &members =
        blocks.constFirst().members;
    if (members.size() != static_cast<qsizetype>(expectedMembers.size())) {
        return QStringLiteral(
            "generated uniform members do not match the Ghostty ABI");
    }
    for (const ExpectedMember &expected : expectedMembers) {
        const auto found =
            std::ranges::find_if(members, [&expected](const auto &member) {
                return member.name == expected.name;
            });
        if (found == members.cend() || found->offset != expected.offset) {
            return QStringLiteral(
                "generated uniform members do not match the Ghostty ABI");
        }
    }

    const QList<QShaderDescription::InOutVariable> samplers =
        shader.description().combinedImageSamplers();
    if (samplers.size() != 1
        || samplers.constFirst().name != QByteArrayView("iChannel0")
        || samplers.constFirst().binding != 1) {
        return QStringLiteral(
            "generated sampler bindings do not match the Ghostty ABI");
    }
    return {};
}

QByteArray readValidCachedShader(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        return {};
    }
    const QShader shader = QShader::fromSerialized(bytes);
    return shaderContractDiagnostic(shader).isEmpty() ? bytes : QByteArray{};
}

bool writeCachedShader(const QString &path, const QShader &shader,
                       QString *diagnostic)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        *diagnostic =
            QStringLiteral("custom-shader: unable to write cache entry %1: %2")
                .arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray serialized = shader.serialized();
    if (file.write(serialized) != serialized.size() || !file.commit()) {
        *diagnostic =
            QStringLiteral("custom-shader: unable to commit cache entry %1: %2")
                .arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    return true;
}

QShader bakeShader(const QByteArray &source, const QString &sourcePath,
                   TerminalCustomShaderCompileMode mode, QString *diagnostic)
{
    const QByteArrayView lifecyclePrefix =
        mode == TerminalCustomShaderCompileMode::PaneEnter
        ? QByteArrayView("\n#define mainImage ghosttyQtPaneEnterMainImage\n")
        : mode == TerminalCustomShaderCompileMode::PaneExit
        ? QByteArrayView("\n#define mainImage ghosttyQtPaneExitMainImage\n")
        : QByteArrayView{};
    const QByteArrayView lifecycleSuffix =
        mode == TerminalCustomShaderCompileMode::PaneEnter
        ? QByteArrayView(R"glsl(
#undef mainImage
void mainImage(out vec4 color, in vec2 fragCoord)
{
    if (iPaneTransition.y == float(PANETRANSITION_ENTER)) {
        ghosttyQtPaneEnterMainImage(color, fragCoord);
    } else {
        color = texture(iChannel0, fragCoord / iResolution.xy);
    }
}
)glsl")
        : mode == TerminalCustomShaderCompileMode::PaneExit
        ? QByteArrayView(R"glsl(
#undef mainImage
void mainImage(out vec4 color, in vec2 fragCoord)
{
    if (iPaneTransition.y == float(PANETRANSITION_EXIT)) {
        ghosttyQtPaneExitMainImage(color, fragCoord);
    } else {
        color = texture(iChannel0, fragCoord / iResolution.xy);
    }
}
)glsl")
        : QByteArrayView{};
    QByteArray wrapped;
    wrapped.reserve(static_cast<qsizetype>(sizeof(kShaderPrefix))
                    + lifecyclePrefix.size() + source.size()
                    + lifecycleSuffix.size()
                    + static_cast<qsizetype>(sizeof(kShaderSuffix)));
    wrapped.append(kShaderPrefix);
    wrapped.append(lifecyclePrefix);
    wrapped.append(source);
    wrapped.append(lifecycleSuffix);
    wrapped.append(kShaderSuffix);

    QShaderBaker baker;
    baker.setSourceString(wrapped, QShader::FragmentStage, sourcePath);
    baker.setGeneratedShaders({
        {QShader::SpirvShader, QShaderVersion(100)},
        {QShader::GlslShader, QShaderVersion(330)},
        {QShader::GlslShader, QShaderVersion(430)},
        {QShader::GlslShader, QShaderVersion(300, QShaderVersion::GlslEs)},
    });
    baker.setGeneratedShaderVariants({QShader::StandardShader});
    baker.setSpirvOptions(QShaderBaker::SpirvOption::StripDebugAndVarInfo);
    baker.setBreakOnShaderTranslationError(false);
    QShader shader = baker.bake();
    const QString contractDiagnostic = shaderContractDiagnostic(shader);
    if (!contractDiagnostic.isEmpty()) {
        const QString bakerDiagnostic = baker.errorMessage().trimmed();
        *diagnostic = QStringLiteral("custom-shader: %1: %2")
                          .arg(QDir::toNativeSeparators(sourcePath),
                               bakerDiagnostic.isEmpty() ? contractDiagnostic
                                                         : bakerDiagnostic);
        return {};
    }
    return shader;
}

} // namespace

TerminalCustomShaderCompileResult
compileTerminalCustomShaders(const TerminalCustomShaderOptions &options,
                             const QString &cacheDirectory,
                             TerminalCustomShaderCompileMode mode)
{
    const auto totalStarted = Clock::now();
    TerminalCustomShaderCompileResult result;
    result.metrics.sourceCount = static_cast<int>(options.sources.size());

    if (options.sources.isEmpty()) {
        result.metrics.totalTime = Clock::now() - totalStarted;
        return result;
    }

    const QString effectiveCacheDirectory =
        cacheDirectory.isEmpty() ? defaultCacheDirectory() : cacheDirectory;
    if (!ensurePrivateCacheDirectory(effectiveCacheDirectory,
                                     &result.diagnostic)) {
        result.metrics.totalTime = Clock::now() - totalStarted;
        return result;
    }

    result.stages.reserve(options.sources.size());
    for (const GhosttyConfigPath &sourceSpec : options.sources) {
        const auto readStarted = Clock::now();
        QFile sourceFile(sourceSpec.path);
        if (!sourceFile.open(QIODevice::ReadOnly)) {
            result.metrics.sourceReadTime += Clock::now() - readStarted;
            if (sourceSpec.optional
                && sourceFile.error() == QFileDevice::FileError::OpenError
                && !QFileInfo::exists(sourceSpec.path)) {
                continue;
            }
            result.diagnostic =
                QStringLiteral("custom-shader: unable to read %1: %2")
                    .arg(pathForDiagnostic(sourceSpec),
                         sourceFile.errorString());
            result.stages.clear();
            result.metrics.totalTime = Clock::now() - totalStarted;
            return result;
        }
        if (sourceFile.size() > kMaximumShaderBytes) {
            result.metrics.sourceReadTime += Clock::now() - readStarted;
            result.diagnostic =
                QStringLiteral("custom-shader: %1 exceeds the 4 MiB limit")
                    .arg(pathForDiagnostic(sourceSpec));
            result.stages.clear();
            result.metrics.totalTime = Clock::now() - totalStarted;
            return result;
        }
        const QByteArray source = sourceFile.read(kMaximumShaderBytes + 1);
        result.metrics.sourceReadTime += Clock::now() - readStarted;
        if (source.size() > kMaximumShaderBytes
            || sourceFile.error() != QFileDevice::NoError) {
            result.diagnostic =
                QStringLiteral("custom-shader: unable to read %1: %2")
                    .arg(pathForDiagnostic(sourceSpec),
                         sourceFile.errorString());
            result.stages.clear();
            result.metrics.totalTime = Clock::now() - totalStarted;
            return result;
        }

        const QByteArray key = cacheKeyForSource(source, mode);
        const QString qsbPath = QDir(effectiveCacheDirectory)
                                    .filePath(QString::fromLatin1(key)
                                              + QStringLiteral(".frag.qsb"));
        const QByteArray cachedShader = readValidCachedShader(qsbPath);
        if (!cachedShader.isEmpty()) {
            ++result.metrics.cacheHitCount;
            result.stages.append({
                .sourcePath = sourceSpec.path,
                .qsbPath = qsbPath,
                .cacheKey = key,
                .serializedShader = cachedShader,
            });
            continue;
        }

        const auto bakeStarted = Clock::now();
        QShader shader =
            bakeShader(source, sourceSpec.path, mode, &result.diagnostic);
        result.metrics.bakeTime += Clock::now() - bakeStarted;
        if (!shader.isValid()
            || !writeCachedShader(qsbPath, shader, &result.diagnostic)) {
            result.stages.clear();
            result.metrics.totalTime = Clock::now() - totalStarted;
            return result;
        }
        ++result.metrics.compiledCount;
        const QByteArray serializedShader = shader.serialized();
        result.stages.append({
            .sourcePath = sourceSpec.path,
            .qsbPath = qsbPath,
            .cacheKey = key,
            .serializedShader = serializedShader,
        });
    }

    result.metrics.totalTime = Clock::now() - totalStarted;
    return result;
}

struct TerminalCustomShaderCompileBroker::State {
    struct Job {
        TerminalCustomShaderOptions options;
        QString cacheDirectory;
        TerminalCustomShaderCompileMode mode =
            TerminalCustomShaderCompileMode::Persistent;
        QVector<PendingCompletion> current;
        QVector<PendingCompletion> queued;
        bool launched = false;
    };

    mutable QMutex mutex;
    QHash<QByteArray, Job> jobs;
};

TerminalCustomShaderCompileBroker::TerminalCustomShaderCompileBroker(
    QObject *parent)
    : QObject(parent)
    , state_(std::make_shared<State>())
{}

void TerminalCustomShaderCompileBroker::request(
    const TerminalCustomShaderOptions &options, QObject *context,
    Completion completion, const QString &cacheDirectory,
    TerminalCustomShaderCompileMode mode)
{
    if (context == nullptr || !completion) {
        return;
    }

    QByteArray key = requestKey(options, mode);
    key.append('\0');
    key.append(cacheDirectory.toUtf8());
    {
        QMutexLocker locker(&state_->mutex);
        auto existing = state_->jobs.find(key);
        if (existing != state_->jobs.end()) {
            QVector<PendingCompletion> &callbacks =
                existing->launched ? existing->queued : existing->current;
            callbacks.append(
                {QPointer<QObject>(context), std::move(completion)});
            return;
        }
        State::Job job;
        job.options = options;
        job.cacheDirectory = cacheDirectory;
        job.mode = mode;
        job.current.append({QPointer<QObject>(context), std::move(completion)});
        state_->jobs.insert(key, std::move(job));
    }

    const QPointer<TerminalCustomShaderCompileBroker> broker(this);
    QMetaObject::invokeMethod(
        this,
        [broker, key] {
            if (broker != nullptr) {
                broker->launch(key);
            }
        },
        Qt::QueuedConnection);
}

void TerminalCustomShaderCompileBroker::launch(const QByteArray &key)
{
    TerminalCustomShaderOptions options;
    QString cacheDirectory;
    TerminalCustomShaderCompileMode mode =
        TerminalCustomShaderCompileMode::Persistent;
    {
        QMutexLocker locker(&state_->mutex);
        auto job = state_->jobs.find(key);
        if (job == state_->jobs.end() || job->launched) {
            return;
        }
        job->launched = true;
        options = job->options;
        cacheDirectory = job->cacheDirectory;
        mode = job->mode;
    }

    const std::shared_ptr<State> state = state_;
    const QPointer<TerminalCustomShaderCompileBroker> broker(this);
    QThreadPool::globalInstance()->start([state, broker, key, options,
                                          cacheDirectory, mode] {
        TerminalCustomShaderCompileResult result =
            compileTerminalCustomShaders(options, cacheDirectory, mode);
        if (broker == nullptr) {
            QMutexLocker locker(&state->mutex);
            state->jobs.remove(key);
            return;
        }
        QMetaObject::invokeMethod(
            broker, [state, broker, key, result = std::move(result)]() mutable {
                QVector<PendingCompletion> callbacks;
                bool rerun = false;
                {
                    QMutexLocker locker(&state->mutex);
                    auto job = state->jobs.find(key);
                    if (job == state->jobs.end()) {
                        return;
                    }
                    callbacks = std::move(job->current);
                    if (job->queued.isEmpty()) {
                        state->jobs.erase(job);
                    } else {
                        job->current = std::move(job->queued);
                        job->queued.clear();
                        job->launched = false;
                        rerun = true;
                    }
                }
                if (broker == nullptr) {
                    return;
                }
                for (PendingCompletion &pending : callbacks) {
                    if (pending.context != nullptr && pending.completion) {
                        pending.completion(result);
                    }
                }
                if (rerun && broker != nullptr) {
                    QMetaObject::invokeMethod(
                        broker,
                        [broker, key] {
                            if (broker != nullptr) {
                                broker->launch(key);
                            }
                        },
                        Qt::QueuedConnection);
                }
            });
    });
}

int TerminalCustomShaderCompileBroker::inFlightCount() const
{
    QMutexLocker locker(&state_->mutex);
    return static_cast<int>(state_->jobs.size());
}
