#include "renderdoc_capture.h"
#include "terminal_custom_shader_compiler.h"
#include "terminal_custom_shader_pipeline.h"
#include "terminal_custom_shader_qsg.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickGraphicsConfiguration>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleRectNode>
#include <QScopeGuard>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <rhi/qrhi.h>

#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanInstance>
#define GHOSTTY_QT_BENCH_HAS_VULKAN 1
#else
#define GHOSTTY_QT_BENCH_HAS_VULKAN 0
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>

namespace {

constexpr int maximumPassCount = 8;
constexpr std::array<int, 5> passCounts{0, 1, 2, 4, 8};
constexpr int maximumMeasurementRounds = 2;

enum class Renderer {
    Legacy,
    Retained,
};

enum class Workload {
    SourceDirty,
    EffectOnly,
};

QStringView rendererName(Renderer renderer)
{
    switch (renderer) {
    case Renderer::Legacy: return u"legacy";
    case Renderer::Retained: return u"retained";
    }
    return {};
}

QStringView workloadName(Workload workload)
{
    switch (workload) {
    case Workload::SourceDirty: return u"source-dirty";
    case Workload::EffectOnly: return u"effect-only";
    }
    return {};
}

struct PassTransform {
    double scale = 1.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

// The per-pass scale makes the translations noncommutative: reordering two
// stages changes the expected pixel even though every individual operation is
// a simple affine color transform.
constexpr std::array<PassTransform, maximumPassCount> passTransforms{{
    {.scale = 0.75, .red = 0.0625},
    {.scale = 0.80, .green = 0.0625},
    {.scale = 0.85, .blue = 0.0625},
    {.scale = 0.90, .red = 0.125},
    {.scale = 0.70, .green = 0.09375},
    {.scale = 0.95, .blue = 0.046875},
    {.scale = 0.82, .red = 0.078125},
    {.scale = 0.88, .green = 0.03125},
}};

constexpr std::array<QByteArrayView, maximumPassCount> shaderTransforms{{
    "terminalColor.rgb * 0.75 + vec3(0.0625, 0.0, 0.0)",
    "terminalColor.rgb * 0.80 + vec3(0.0, 0.0625, 0.0)",
    "terminalColor.rgb * 0.85 + vec3(0.0, 0.0, 0.0625)",
    "terminalColor.rgb * 0.90 + vec3(0.125, 0.0, 0.0)",
    "terminalColor.rgb * 0.70 + vec3(0.0, 0.09375, 0.0)",
    "terminalColor.rgb * 0.95 + vec3(0.0, 0.0, 0.046875)",
    "terminalColor.rgb * 0.82 + vec3(0.078125, 0.0, 0.0)",
    "terminalColor.rgb * 0.88 + vec3(0.0, 0.03125, 0.0)",
}};

constexpr std::array<double, 3> effectFrameBias{
    0.015625,
    0.0078125,
    0.00390625,
};

struct Summary {
    double minimumMicroseconds = 0.0;
    double medianMicroseconds = 0.0;
    double p90Microseconds = 0.0;
    double meanMicroseconds = 0.0;
    double maximumMicroseconds = 0.0;
};

struct FrameTiming {
    qint64 cpuRecordNanoseconds = 0;
    qint64 cpuCompletionNanoseconds = 0;
    std::optional<qint64> gpuNanoseconds;
};

Summary summarize(QVector<qint64> samples)
{
    std::ranges::sort(samples);
    const qsizetype count = samples.size();
    const qsizetype p90 =
        std::min(count - 1,
                 static_cast<qsizetype>(
                     std::ceil(static_cast<double>(count) * 0.9) - 1.0));
    const qint64 total =
        std::accumulate(samples.cbegin(), samples.cend(), qint64{0});
    const auto microseconds = [](qint64 nanoseconds) {
        return static_cast<double>(nanoseconds) / 1'000.0;
    };
    const double medianNanoseconds = count % 2 == 0
        ? (static_cast<double>(samples.at(count / 2 - 1))
           + static_cast<double>(samples.at(count / 2)))
            / 2.0
        : static_cast<double>(samples.at(count / 2));
    return {
        .minimumMicroseconds = microseconds(samples.constFirst()),
        .medianMicroseconds = medianNanoseconds / 1'000.0,
        .p90Microseconds = microseconds(samples.at(p90)),
        .meanMicroseconds = microseconds(total) / static_cast<double>(count),
        .maximumMicroseconds = microseconds(samples.constLast()),
    };
}

QColor sourceColorForFrame(quint64 frame)
{
    return frame % 2 == 0 ? QColor(48, 80, 112) : QColor(96, 64, 128);
}

class BenchmarkSourceItem : public QQuickItem {
    Q_OBJECT

public:
    explicit BenchmarkSourceItem(QQuickItem *parent = nullptr)
        : QQuickItem(parent)
    {
        setFlag(QQuickItem::ItemHasContents);
    }

    void setFrame(quint64 frame)
    {
        frame_ = frame;
        color_ = sourceColorForFrame(frame);
        update();
    }

    [[nodiscard]] quint64 paintedFrame() const noexcept
    {
        return paintedFrame_.load(std::memory_order_acquire);
    }

    [[nodiscard]] quint64 paintCount() const noexcept
    {
        return paintCount_.load(std::memory_order_acquire);
    }

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override
    {
        auto *node = static_cast<QSGSimpleRectNode *>(oldNode);
        if (node == nullptr) node = new QSGSimpleRectNode;
        node->setRect(boundingRect());
        node->setColor(color_);
        paintedFrame_.store(frame_, std::memory_order_release);
        paintCount_.fetch_add(1, std::memory_order_release);
        return node;
    }

private:
    QColor color_ = sourceColorForFrame(0);
    quint64 frame_ = 0;
    std::atomic<quint64> paintedFrame_{std::numeric_limits<quint64>::max()};
    std::atomic<quint64> paintCount_{0};
};

class BenchmarkUniformProvider final
    : public QObject,
      public TerminalCustomShaderUniformProvider {
    Q_OBJECT
    Q_INTERFACES(TerminalCustomShaderUniformProvider)

public:
    explicit BenchmarkUniformProvider(QSize physicalSize,
                                      QObject *parent = nullptr)
        : QObject(parent)
        , physicalSize_(physicalSize)
    {
        setFrame(0);
    }

    [[nodiscard]] TerminalCustomShaderUniformSnapshot
    terminalCustomShaderUniformSnapshot(int) const override
    {
        QMutexLocker locker(&mutex_);
        return uniforms_;
    }

    void terminalCustomShaderEffectAttached(TerminalCustomShaderEffect *effect,
                                            int) override
    {
        if (effect == nullptr
            || std::ranges::any_of(
                effects_,
                [effect](
                    const QPointer<TerminalCustomShaderEffect> &candidate) {
                    return candidate == effect;
                })) {
            return;
        }
        effects_.append(effect);
    }

    void terminalCustomShaderEffectDetached(TerminalCustomShaderEffect *effect,
                                            int) override
    {
        effects_.removeIf(
            [effect](const QPointer<TerminalCustomShaderEffect> &candidate) {
                return candidate == nullptr || candidate == effect;
            });
    }

    void terminalCustomShaderPipelineAttached(
        TerminalCustomShaderPipelineEffect *effect) override
    {
        pipeline_ = effect;
    }

    void terminalCustomShaderPipelineDetached(
        TerminalCustomShaderPipelineEffect *effect) override
    {
        if (pipeline_ == effect) pipeline_.clear();
    }

    void requestEffectFrame(quint64 frame)
    {
        setFrame(frame);
        effects_.removeIf(
            [](const QPointer<TerminalCustomShaderEffect> &effect) {
                return effect == nullptr;
            });
        for (TerminalCustomShaderEffect *effect : std::as_const(effects_)) {
            effect->update();
        }
        if (pipeline_ != nullptr) pipeline_->update();
    }

    [[nodiscard]] TerminalCustomShaderPipelineEffect *pipeline() const noexcept
    {
        return pipeline_;
    }

private:
    void setFrame(quint64 frame)
    {
        auto uniforms = std::make_shared<TerminalCustomShaderUniforms>();
        uniforms->resolution = {
            static_cast<float>(physicalSize_.width()),
            static_cast<float>(physicalSize_.height()),
            1.0F,
        };
        uniforms->frame = static_cast<std::int32_t>(frame % 2);
        uniforms->time = static_cast<float>(frame % 2);
        QMutexLocker locker(&mutex_);
        uniforms_ = std::move(uniforms);
    }

    QSize physicalSize_;
    mutable QMutex mutex_;
    TerminalCustomShaderUniformSnapshot uniforms_;
    QVector<QPointer<TerminalCustomShaderEffect>> effects_;
    QPointer<TerminalCustomShaderPipelineEffect> pipeline_;
};

bool writeBenchmarkShaders(const QDir &directory,
                           TerminalCustomShaderOptions *options, QString *error)
{
    options->sources.clear();
    for (int index = 0; index < maximumPassCount; ++index) {
        const QString path =
            directory.filePath(QStringLiteral("pass-%1.glsl").arg(index));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            *error = QStringLiteral("unable to create %1: %2")
                         .arg(path, file.errorString());
            return false;
        }
        const QByteArray source =
            QByteArrayLiteral(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec4 terminalColor = texture(iChannel0, uv);
    float framePhase = mod(float(iFrame), 2.0);
    color = vec4()glsl")
            + QByteArray(shaderTransforms.at(static_cast<std::size_t>(index)))
            + QByteArrayLiteral(
                " + framePhase * vec3(0.015625, 0.0078125, 0.00390625), "
                "terminalColor.a);\n"
                "}\n");
        if (file.write(source) != source.size()) {
            *error = QStringLiteral("unable to write %1: %2")
                         .arg(path, file.errorString());
            return false;
        }
        options->sources.append({.path = path, .optional = false});
    }
    return true;
}

QString benchmarkQml(Renderer renderer, int passCount)
{
    QString qml = QStringLiteral("import QtQuick\n"
                                 "import GhosttyShaderBench 1.0\n"
                                 "Item {\n"
                                 "  width: benchWidth\n"
                                 "  height: benchHeight\n");
    if (passCount == 0) {
        qml += QStringLiteral("  BenchmarkSource {\n"
                              "    objectName: \"benchmarkSource\"\n"
                              "    anchors.fill: parent\n"
                              "  }\n"
                              "}\n");
        return qml;
    }

    if (renderer == Renderer::Retained) {
        qml +=
            QStringLiteral("  layer.enabled: true\n"
                           "  layer.live: true\n"
                           "  layer.smooth: true\n"
                           "  layer.textureMirroring: "
                           "ShaderEffectSource.NoMirroring\n"
                           "  layer.textureSize: "
                           "Qt.size(benchTextureWidth, benchTextureHeight)\n"
                           "  layer.effect: Component {\n"
                           "    TerminalCustomShaderPipelineEffect {\n"
                           "      objectName: \"benchmarkPipeline\"\n"
                           "      shaderStages: benchmarkShaderStages\n"
                           "      uniformProvider: benchmarkUniformProvider\n"
                           "    }\n"
                           "  }\n"
                           "  BenchmarkSource {\n"
                           "    objectName: \"benchmarkSource\"\n"
                           "    anchors.fill: parent\n"
                           "  }\n"
                           "}\n");
        return qml;
    }

    for (int stage = passCount - 1; stage >= 0; --stage) {
        qml +=
            QStringLiteral("  Item {\n"
                           "    anchors.fill: parent\n"
                           "    layer.enabled: true\n"
                           "    layer.live: true\n"
                           "    layer.smooth: true\n"
                           "    layer.textureMirroring: "
                           "ShaderEffectSource.NoMirroring\n"
                           "    layer.textureSize: "
                           "Qt.size(benchTextureWidth, benchTextureHeight)\n"
                           "    layer.effect: Component {\n"
                           "      TerminalCustomShaderEffect {\n"
                           "        fragmentShaderFileName: shaderPath%1\n"
                           "        fragmentShaderData: shaderData%1\n"
                           "        uniformProvider: benchmarkUniformProvider\n"
                           "        stageIndex: %1\n"
                           "      }\n"
                           "    }\n")
                .arg(stage);
    }
    qml += QStringLiteral("  BenchmarkSource {\n"
                          "    objectName: \"benchmarkSource\"\n"
                          "    anchors.fill: parent\n"
                          "  }\n");
    for (int stage = 0; stage < passCount; ++stage) {
        qml += QStringLiteral("  }\n");
    }
    qml += QStringLiteral("}\n");
    return qml;
}

class Scenario final {
public:
    bool initialize(QQmlEngine *engine, QQuickWindow *window,
                    BenchmarkUniformProvider *provider,
                    const QVector<TerminalCustomShaderStage> &stages,
                    Renderer renderer, int passCount, QSize logicalSize,
                    QSize physicalSize, QString *error)
    {
        renderer_ = renderer;
        passCount_ = passCount;
        provider_ = provider;
        context_ = std::make_unique<QQmlContext>(engine->rootContext(), engine);
        context_->setContextProperty(QStringLiteral("benchWidth"),
                                     logicalSize.width());
        context_->setContextProperty(QStringLiteral("benchHeight"),
                                     logicalSize.height());
        context_->setContextProperty(QStringLiteral("benchTextureWidth"),
                                     physicalSize.width());
        context_->setContextProperty(QStringLiteral("benchTextureHeight"),
                                     physicalSize.height());
        context_->setContextProperty(QStringLiteral("benchmarkUniformProvider"),
                                     provider);
        context_->setContextProperty(QStringLiteral("benchmarkShaderStages"),
                                     terminalCustomShaderStagesToVariantList(
                                         stages.sliced(0, passCount)));
        for (int stage = 0; stage < passCount; ++stage) {
            context_->setContextProperty(
                QStringLiteral("shaderPath%1").arg(stage),
                stages.at(stage).qsbPath);
            context_->setContextProperty(
                QStringLiteral("shaderData%1").arg(stage),
                stages.at(stage).serializedShader);
        }

        QQmlComponent component(engine);
        const QByteArray qml = benchmarkQml(renderer, passCount).toUtf8();
        component.setData(qml, QUrl());
        if (!component.isReady()) {
            *error = component.errorString();
            if (error->isEmpty()) {
                *error = QStringLiteral(
                    "benchmark QML component did not become ready");
            }
            return false;
        }
        QObject *const created = component.create(context_.get());
        if (created == nullptr) {
            *error = component.errorString();
            return false;
        }
        root_.reset(qobject_cast<QQuickItem *>(created));
        if (root_ == nullptr) {
            delete created;
            *error = QStringLiteral("benchmark QML root is not an Item");
            return false;
        }
        source_ = root_->findChild<BenchmarkSourceItem *>(
            QStringLiteral("benchmarkSource"));
        if (source_ == nullptr) {
            *error =
                QStringLiteral("benchmark QML did not create its source item");
            return false;
        }
        source_->setFrame(0);
        root_->setParentItem(window->contentItem());
        return true;
    }

    bool render(QQuickRenderControl *renderControl, QRhi *rhi,
                QRhiTexture *colorBuffer, Workload workload, quint64 frame,
                FrameTiming *timing, bool validateOutput, QString *error)
    {
        quint64 expectedSourceFrame = 0;
        quint64 expectedEffectFrame = 0;
        if (workload == Workload::SourceDirty) {
            expectedSourceFrame = frame;
            source_->setFrame(frame);
        } else {
            expectedEffectFrame = frame;
            provider_->requestEffectFrame(frame);
        }

        QRhiReadbackResult readback;
        QElapsedTimer recordTimer;
        if (timing != nullptr) recordTimer.start();
        renderControl->polishItems();
        renderControl->beginFrame();
        renderControl->sync();
        renderControl->render();
        QRhiCommandBuffer *const commandBuffer = renderControl->commandBuffer();
        if (commandBuffer == nullptr) {
            *error = QStringLiteral(
                "QQuickRenderControl did not provide a command buffer");
            renderControl->endFrame();
            return false;
        }
        if (timing != nullptr) {
            timing->cpuRecordNanoseconds = recordTimer.nsecsElapsed();
        }
        if (validateOutput) {
            QRhiResourceUpdateBatch *const readbackBatch =
                rhi->nextResourceUpdateBatch();
            readbackBatch->readBackTexture(colorBuffer, &readback);
            commandBuffer->resourceUpdate(readbackBatch);
        }

        QElapsedTimer completionTimer;
        if (timing != nullptr) completionTimer.start();
        renderControl->endFrame();
        if (timing != nullptr) {
            timing->cpuCompletionNanoseconds = completionTimer.nsecsElapsed();
            if (rhi->isFeatureSupported(QRhi::Timestamps)) {
                const double gpuSeconds = commandBuffer->lastCompletedGpuTime();
                if (std::isfinite(gpuSeconds) && gpuSeconds > 0.0) {
                    timing->gpuNanoseconds =
                        qRound64(gpuSeconds * 1'000'000'000.0);
                }
            }
        }

        if (source_->paintedFrame() != expectedSourceFrame) {
            *error = QStringLiteral(
                         "source frame %1 was not rendered (last painted %2)")
                         .arg(expectedSourceFrame)
                         .arg(source_->paintedFrame());
            return false;
        }
        if (validateOutput) {
            if (readback.data.isEmpty() || readback.pixelSize.isEmpty()) {
                *error = QStringLiteral(
                    "offscreen texture readback returned no pixels");
                return false;
            }
            const QImage wrapper(
                reinterpret_cast<const uchar *>(readback.data.constData()),
                readback.pixelSize.width(), readback.pixelSize.height(),
                QImage::Format_RGBA8888_Premultiplied);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
            const QImage image = rhi->isYUpInFramebuffer()
                ? wrapper.flipped(Qt::Vertical)
                : wrapper.copy();
#else
            const QImage image = rhi->isYUpInFramebuffer()
                ? wrapper.mirrored(false, true)
                : wrapper.copy();
#endif
            if (!validatePixel(image, expectedSourceFrame, expectedEffectFrame,
                               error)) {
                return false;
            }
        }

        if (renderer_ == Renderer::Retained && passCount_ > 0) {
            TerminalCustomShaderPipelineEffect *const pipeline =
                provider_->pipeline();
            if (pipeline == nullptr) {
                *error =
                    QStringLiteral("retained pipeline effect was not attached");
                return false;
            }
            const TerminalCustomShaderPipelineSnapshot snapshot =
                pipeline->renderSnapshot();
            if (!snapshot.diagnostic.isEmpty()) {
                *error = snapshot.diagnostic;
                return false;
            }
            const int expectedTargets =
                terminalCustomShaderPipelineTargetCount(passCount_);
            if (snapshot.passCount != passCount_
                || snapshot.liveTargetCount != expectedTargets) {
                *error =
                    QStringLiteral("retained telemetry mismatch: passes=%1/%2, "
                                   "targets=%3/%4")
                        .arg(snapshot.passCount)
                        .arg(passCount_)
                        .arg(snapshot.liveTargetCount)
                        .arg(expectedTargets);
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<TerminalCustomShaderPipelineSnapshot>
    pipelineSnapshot() const
    {
        if (renderer_ != Renderer::Retained || passCount_ == 0) {
            return std::nullopt;
        }
        TerminalCustomShaderPipelineEffect *const pipeline =
            provider_->pipeline();
        if (pipeline == nullptr) return std::nullopt;
        return pipeline->renderSnapshot();
    }

    [[nodiscard]] quint64 sourcePaintCount() const noexcept
    {
        return source_->paintCount();
    }

private:
    bool validatePixel(const QImage &image, quint64 sourceFrame,
                       quint64 effectFrame, QString *error) const
    {
        const QColor source = sourceColorForFrame(sourceFrame);
        std::array<double, 3> transformed{{
            static_cast<double>(source.red()) / 255.0,
            static_cast<double>(source.green()) / 255.0,
            static_cast<double>(source.blue()) / 255.0,
        }};
        const double framePhase = static_cast<double>(effectFrame % 2);
        for (int stage = 0; stage < passCount_; ++stage) {
            const PassTransform transform =
                passTransforms.at(static_cast<std::size_t>(stage));
            transformed.at(0) = transformed.at(0) * transform.scale
                + transform.red + framePhase * effectFrameBias.at(0);
            transformed.at(1) = transformed.at(1) * transform.scale
                + transform.green + framePhase * effectFrameBias.at(1);
            transformed.at(2) = transformed.at(2) * transform.scale
                + transform.blue + framePhase * effectFrameBias.at(2);
        }
        const auto expectedChannel = [](double value) {
            return qRound(std::clamp(value, 0.0, 1.0) * 255.0);
        };
        const std::array<int, 4> expected{{
            expectedChannel(transformed.at(0)),
            expectedChannel(transformed.at(1)),
            expectedChannel(transformed.at(2)),
            255,
        }};
        const QColor actual =
            image.pixelColor(image.width() / 2, image.height() / 2);
        const std::array<int, 4> observed{{
            actual.red(),
            actual.green(),
            actual.blue(),
            actual.alpha(),
        }};
        constexpr int tolerance = 6;
        for (std::size_t channel = 0; channel < expected.size(); ++channel) {
            if (std::abs(observed.at(channel) - expected.at(channel))
                > tolerance) {
                *error = QStringLiteral(
                             "ordered pass validation failed for source frame "
                             "%1/effect frame %2: expected "
                             "rgba=(%3,%4,%5,%6), got (%7,%8,%9,%10)")
                             .arg(sourceFrame)
                             .arg(effectFrame)
                             .arg(expected.at(0))
                             .arg(expected.at(1))
                             .arg(expected.at(2))
                             .arg(expected.at(3))
                             .arg(observed.at(0))
                             .arg(observed.at(1))
                             .arg(observed.at(2))
                             .arg(observed.at(3));
                return false;
            }
        }
        return true;
    }

    Renderer renderer_ = Renderer::Legacy;
    int passCount_ = 0;
    BenchmarkUniformProvider *provider_ = nullptr;
    std::unique_ptr<QQmlContext> context_;
    std::unique_ptr<QQuickItem> root_;
    BenchmarkSourceItem *source_ = nullptr;
};

struct CaptureSelection {
    Renderer renderer = Renderer::Retained;
    Workload workload = Workload::EffectOnly;
    int passCount = maximumPassCount;

    [[nodiscard]] QString name() const
    {
        return QStringLiteral("%1:%2:%3")
            .arg(rendererName(renderer))
            .arg(workloadName(workload))
            .arg(passCount);
    }
};

struct ScenarioResult {
    Renderer renderer = Renderer::Legacy;
    Workload workload = Workload::SourceDirty;
    int passCount = 0;
    Summary cpuRecordTiming;
    Summary cpuCompletionTiming;
    Summary cpuTotalTiming;
    std::optional<Summary> gpuTiming;
    int validGpuSampleCount = 0;
    QVector<qint64> baselineCpuTotalSamples;
    QVector<qint64> baselineGpuSamples;
    quint64 sourcePaintDelta = 0;
    std::optional<TerminalCustomShaderPipelineSnapshot> before;
    std::optional<TerminalCustomShaderPipelineSnapshot> after;
};

struct ScenarioAccumulator {
    Renderer renderer = Renderer::Legacy;
    Workload workload = Workload::SourceDirty;
    int passCount = 0;
    QVector<qint64> cpuRecordSamples;
    QVector<qint64> cpuCompletionSamples;
    QVector<qint64> cpuTotalSamples;
    QVector<qint64> gpuSamples;
    quint64 sourcePaintDelta = 0;
    int validationReadbackCount = 0;
    std::uint64_t renderedFrameCount = 0;
    std::uint64_t recordedDrawCount = 0;
    std::uint64_t targetCreateCount = 0;
    std::uint64_t targetDestroyCount = 0;
    std::uint64_t pipelineCreateCount = 0;
    std::uint64_t sourceBindingUpdateCount = 0;
    std::optional<TerminalCustomShaderPipelineSnapshot> latestSnapshot;
};

struct WorkloadBaseline {
    Workload workload = Workload::SourceDirty;
    Summary cpuTotalTiming;
    std::optional<Summary> gpuTiming;
    double cpuRendererGapMicroseconds = 0.0;
    std::optional<double> gpuRendererGapMicroseconds;
    qsizetype pooledCpuSampleCount = 0;
    qsizetype pooledGpuSampleCount = 0;
};

std::optional<int> nonNegativeInteger(const QString &text)
{
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value < 0) return std::nullopt;
    return value;
}

std::optional<int> positiveInteger(const QString &text)
{
    const std::optional<int> value = nonNegativeInteger(text);
    if (!value.has_value() || *value == 0) return std::nullopt;
    return value;
}

std::optional<CaptureSelection> parseCaptureSelection(const QString &text,
                                                      QString *error)
{
    const QStringList parts = text.trimmed().split(u':');
    if (parts.size() != 3) {
        *error = QStringLiteral(
            "renderdoc-capture must be renderer:workload:passes");
        return std::nullopt;
    }

    CaptureSelection selection;
    if (parts.at(0) == QStringLiteral("legacy")) {
        selection.renderer = Renderer::Legacy;
    } else if (parts.at(0) == QStringLiteral("retained")) {
        selection.renderer = Renderer::Retained;
    } else {
        *error =
            QStringLiteral("RenderDoc renderer must be legacy or retained");
        return std::nullopt;
    }

    if (parts.at(1) == QStringLiteral("source-dirty")) {
        selection.workload = Workload::SourceDirty;
    } else if (parts.at(1) == QStringLiteral("effect-only")) {
        selection.workload = Workload::EffectOnly;
    } else {
        *error = QStringLiteral(
            "RenderDoc workload must be source-dirty or effect-only");
        return std::nullopt;
    }

    const std::optional<int> selectedPassCount =
        nonNegativeInteger(parts.at(2));
    if (!selectedPassCount.has_value()
        || std::ranges::find(passCounts, *selectedPassCount)
            == passCounts.cend()) {
        *error =
            QStringLiteral("RenderDoc pass count must be 0, 1, 2, 4, or 8");
        return std::nullopt;
    }
    selection.passCount = *selectedPassCount;
    return selection;
}

std::uint64_t counterDelta(std::uint64_t after, std::uint64_t before)
{
    return after >= before ? after - before : 0;
}

constexpr std::size_t rendererIndex(Renderer renderer)
{
    return renderer == Renderer::Legacy ? 0U : 1U;
}

int framesInRound(int totalFrames, int round, int roundCount)
{
    return totalFrames / roundCount
        + (round < totalFrames % roundCount ? 1 : 0);
}

const ScenarioResult *findResult(const QVector<ScenarioResult> &results,
                                 Renderer renderer, Workload workload,
                                 int passCount)
{
    const auto result =
        std::ranges::find_if(results, [=](const ScenarioResult &candidate) {
            return candidate.renderer == renderer
                && candidate.workload == workload
                && candidate.passCount == passCount;
        });
    return result != results.cend() ? std::addressof(*result) : nullptr;
}

std::optional<WorkloadBaseline>
sharedBaseline(const QVector<ScenarioResult> &results, Workload workload)
{
    const ScenarioResult *const legacy =
        findResult(results, Renderer::Legacy, workload, 0);
    const ScenarioResult *const retained =
        findResult(results, Renderer::Retained, workload, 0);
    if (legacy == nullptr || retained == nullptr
        || legacy->baselineCpuTotalSamples.isEmpty()
        || retained->baselineCpuTotalSamples.isEmpty()) {
        return std::nullopt;
    }

    QVector<qint64> cpuSamples = legacy->baselineCpuTotalSamples;
    cpuSamples += retained->baselineCpuTotalSamples;
    WorkloadBaseline baseline{
        .workload = workload,
        .cpuTotalTiming = summarize(cpuSamples),
        .cpuRendererGapMicroseconds =
            std::abs(legacy->cpuTotalTiming.medianMicroseconds
                     - retained->cpuTotalTiming.medianMicroseconds),
        .pooledCpuSampleCount = cpuSamples.size(),
    };

    if (!legacy->baselineGpuSamples.isEmpty()
        && !retained->baselineGpuSamples.isEmpty()) {
        QVector<qint64> gpuSamples = legacy->baselineGpuSamples;
        gpuSamples += retained->baselineGpuSamples;
        baseline.gpuTiming = summarize(gpuSamples);
        baseline.gpuRendererGapMicroseconds =
            std::abs(legacy->gpuTiming->medianMicroseconds
                     - retained->gpuTiming->medianMicroseconds);
        baseline.pooledGpuSampleCount = gpuSamples.size();
    }
    return baseline;
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")) {
        qputenv("QT_SCALE_FACTOR", "1");
    }

    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-custom-shader-rhi"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Ghostty Qt retained-versus-legacy custom-shader offscreen RHI "
        "microbenchmark"));
    parser.addHelpOption();
    const QCommandLineOption warmupOption(
        QStringLiteral("warmup"),
        QStringLiteral("Untimed warm-up frames per scenario."),
        QStringLiteral("count"), QStringLiteral("200"));
    const QCommandLineOption iterationsOption(
        QStringLiteral("iterations"),
        QStringLiteral("Measured frames per scenario."),
        QStringLiteral("count"), QStringLiteral("100"));
    const QCommandLineOption widthOption(
        QStringLiteral("width"), QStringLiteral("Logical viewport width."),
        QStringLiteral("pixels"), QStringLiteral("1280"));
    const QCommandLineOption heightOption(
        QStringLiteral("height"), QStringLiteral("Logical viewport height."),
        QStringLiteral("pixels"), QStringLiteral("720"));
    const QCommandLineOption graphicsApiOption(
        QStringLiteral("graphics-api"),
        QStringLiteral("Graphics API: opengl or vulkan."),
        QStringLiteral("api"), QStringLiteral("opengl"));
    const QCommandLineOption renderDocCaptureOption(
        QStringLiteral("renderdoc-capture"),
        QStringLiteral(
            "Capture one untimed offscreen frame through an already-injected "
            "RenderDoc API. Syntax: renderer:workload:passes; for example, "
            "retained:effect-only:8."),
        QStringLiteral("scenario"));
    const QCommandLineOption renderDocCapturePathOption(
        QStringLiteral("renderdoc-capture-path"),
        QStringLiteral("UTF-8 RenderDoc capture filename template."),
        QStringLiteral("path"));
    parser.addOptions({warmupOption, iterationsOption, widthOption,
                       heightOption, graphicsApiOption, renderDocCaptureOption,
                       renderDocCapturePathOption});
    parser.process(application);

    QString error;
    const std::optional<int> warmup =
        nonNegativeInteger(parser.value(warmupOption));
    const std::optional<int> iterations =
        positiveInteger(parser.value(iterationsOption));
    const std::optional<int> width = positiveInteger(parser.value(widthOption));
    const std::optional<int> height =
        positiveInteger(parser.value(heightOption));
    if (!warmup.has_value() || !iterations.has_value() || !width.has_value()
        || !height.has_value()) {
        QTextStream(stderr)
            << "warmup must be non-negative; iterations, width, and height "
               "must be positive integers\n";
        return 2;
    }

    std::optional<CaptureSelection> captureSelection;
    if (parser.isSet(renderDocCaptureOption)) {
        captureSelection =
            parseCaptureSelection(parser.value(renderDocCaptureOption), &error);
        if (!captureSelection.has_value()) {
            QTextStream(stderr) << error << '\n';
            return 2;
        }
    } else if (parser.isSet(renderDocCapturePathOption)) {
        QTextStream(stderr)
            << "renderdoc-capture-path requires renderdoc-capture\n";
        return 2;
    }

    const QString requestedGraphicsApi =
        parser.value(graphicsApiOption).trimmed().toLower();
    QSGRendererInterface::GraphicsApi graphicsApi =
        QSGRendererInterface::Unknown;
    if (requestedGraphicsApi == QStringLiteral("opengl")) {
        graphicsApi = QSGRendererInterface::OpenGL;
    } else if (requestedGraphicsApi == QStringLiteral("vulkan")) {
#if GHOSTTY_QT_BENCH_HAS_VULKAN
        graphicsApi = QSGRendererInterface::Vulkan;
#else
        QTextStream(stderr)
            << "this build cannot provide the requested Vulkan API\n";
        return 2;
#endif
    } else {
        QTextStream(stderr) << "graphics-api must be either opengl or vulkan\n";
        return 2;
    }
    QQuickWindow::setGraphicsApi(graphicsApi);

    std::unique_ptr<RenderDocCapture> renderDocCapture;
    if (captureSelection.has_value()) {
        renderDocCapture = std::make_unique<RenderDocCapture>();
        if (!renderDocCapture->isAvailable()) {
            QTextStream(stderr) << "unable to initialize RenderDoc capture: "
                                << renderDocCapture->errorString() << '\n';
            return 4;
        }
        if (parser.isSet(renderDocCapturePathOption)
            && !renderDocCapture->setCapturePathTemplate(
                parser.value(renderDocCapturePathOption).toUtf8())) {
            QTextStream(stderr) << "unable to configure RenderDoc capture: "
                                << renderDocCapture->errorString() << '\n';
            return 4;
        }
    }

    qmlRegisterType<BenchmarkSourceItem>("GhosttyShaderBench", 1, 0,
                                         "BenchmarkSource");
    qmlRegisterType<TerminalCustomShaderEffect>("GhosttyShaderBench", 1, 0,
                                                "TerminalCustomShaderEffect");
    qmlRegisterType<TerminalCustomShaderPipelineEffect>(
        "GhosttyShaderBench", 1, 0, "TerminalCustomShaderPipelineEffect");

    QTemporaryDir shaderRoot;
    if (!shaderRoot.isValid()) {
        QTextStream(stderr) << "unable to create shader workspace\n";
        return 1;
    }
    TerminalCustomShaderOptions options;
    const QDir shaderDirectory(shaderRoot.path());
    if (!writeBenchmarkShaders(shaderDirectory, &options, &error)) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    const TerminalCustomShaderCompileResult compiled =
        compileTerminalCustomShaders(
            options, shaderDirectory.filePath(QStringLiteral("qsb-cache")));
    if (!compiled.succeeded() || compiled.stages.size() != maximumPassCount) {
        QTextStream(stderr)
            << (compiled.diagnostic.isEmpty()
                    ? QStringLiteral("custom-shader compiler returned %1 "
                                     "stages, expected %2")
                          .arg(compiled.stages.size())
                          .arg(maximumPassCount)
                    : compiled.diagnostic)
            << '\n';
        return 1;
    }

    const QSize logicalSize(*width, *height);
#if GHOSTTY_QT_BENCH_HAS_VULKAN
    std::unique_ptr<QVulkanInstance> vulkanInstance;
    if (graphicsApi == QSGRendererInterface::Vulkan) {
        vulkanInstance = std::make_unique<QVulkanInstance>();
        QByteArrayList extensions =
            QQuickGraphicsConfiguration::preferredInstanceExtensions();
        if (captureSelection.has_value()) {
            const QByteArray debugUtilsExtension(
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            if (vulkanInstance->supportedExtensions().contains(
                    debugUtilsExtension)) {
                if (!extensions.contains(debugUtilsExtension)) {
                    extensions.append(debugUtilsExtension);
                }
            } else {
                QTextStream(stderr)
                    << "warning: Vulkan does not expose " << debugUtilsExtension
                    << "; RenderDoc labels will be unavailable\n";
            }
        }
        vulkanInstance->setExtensions(extensions);
        if (!vulkanInstance->create()) {
            QTextStream(stderr) << "unable to create Vulkan instance (VkResult "
                                << vulkanInstance->errorCode() << ")\n";
            return 3;
        }
    }
#endif
    QQuickRenderControl renderControl;
    QQuickWindow window(&renderControl);
    window.setColor(Qt::black);
    window.setGeometry(0, 0, logicalSize.width(), logicalSize.height());
    window.contentItem()->setSize(logicalSize);
    QQuickGraphicsConfiguration graphicsConfiguration;
    graphicsConfiguration.setTimestamps(!captureSelection.has_value());
    graphicsConfiguration.setDebugMarkers(captureSelection.has_value());
    window.setGraphicsConfiguration(graphicsConfiguration);
#if GHOSTTY_QT_BENCH_HAS_VULKAN
    if (vulkanInstance != nullptr) {
        window.setVulkanInstance(vulkanInstance.get());
    }
#endif
    if (!renderControl.initialize()) {
        QTextStream(stderr) << "QQuickRenderControl could not initialize "
                            << requestedGraphicsApi << " on platform "
                            << QGuiApplication::platformName() << '\n';
        return 3;
    }
    if (window.rendererInterface()->graphicsApi() != graphicsApi) {
        QTextStream(stderr)
            << requestedGraphicsApi
            << " RHI was requested, but Qt selected graphics API "
            << static_cast<int>(window.rendererInterface()->graphicsApi())
            << '\n';
        renderControl.invalidate();
        return 3;
    }
    QRhi *const rhi = renderControl.rhi();
    if (rhi == nullptr) {
        QTextStream(stderr) << "QQuickRenderControl did not provide a QRhi\n";
        renderControl.invalidate();
        return 1;
    }
    if (captureSelection.has_value()
        && !rhi->isFeatureSupported(QRhi::DebugMarkers)) {
        QTextStream(stderr)
            << "warning: " << rhi->backendName()
            << " does not support QRhi debug markers; the capture will not "
               "contain custom pass labels\n";
    }
    const qreal dpr = window.devicePixelRatio();
    const QSize physicalSize(qMax(1, qRound(logicalSize.width() * dpr)),
                             qMax(1, qRound(logicalSize.height() * dpr)));

    std::unique_ptr<QRhiTexture> colorBuffer(rhi->newTexture(
        QRhiTexture::RGBA8, physicalSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    colorBuffer->setName(
        QByteArrayLiteral("ghostty-qt custom-shader benchmark output"));
    if (!colorBuffer->create()) {
        QTextStream(stderr) << "unable to create offscreen color buffer\n";
        renderControl.invalidate();
        colorBuffer.reset();
        return 1;
    }
    std::unique_ptr<QRhiRenderBuffer> depthStencil(
        rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, physicalSize, 1));
    depthStencil->setName(
        QByteArrayLiteral("ghostty-qt custom-shader benchmark depth-stencil"));
    if (!depthStencil->create()) {
        QTextStream(stderr)
            << "unable to create offscreen depth-stencil buffer\n";
        renderControl.invalidate();
        depthStencil.reset();
        colorBuffer.reset();
        return 1;
    }
    const QRhiTextureRenderTargetDescription targetDescription(
        QRhiColorAttachment(colorBuffer.get()), depthStencil.get());
    std::unique_ptr<QRhiTextureRenderTarget> renderTarget(
        rhi->newTextureRenderTarget(targetDescription));
    std::unique_ptr<QRhiRenderPassDescriptor> renderPassDescriptor(
        renderTarget->newCompatibleRenderPassDescriptor());
    renderPassDescriptor->setName(
        QByteArrayLiteral("ghostty-qt custom-shader benchmark render pass"));
    renderTarget->setRenderPassDescriptor(renderPassDescriptor.get());
    renderTarget->setName(
        QByteArrayLiteral("ghostty-qt custom-shader benchmark target"));
    if (!renderTarget->create()) {
        QTextStream(stderr) << "unable to create offscreen render target\n";
        renderControl.invalidate();
        renderTarget.reset();
        renderPassDescriptor.reset();
        depthStencil.reset();
        colorBuffer.reset();
        return 1;
    }
    QQuickRenderTarget quickRenderTarget =
        QQuickRenderTarget::fromRhiRenderTarget(renderTarget.get());
    quickRenderTarget.setDevicePixelRatio(dpr);
    window.setRenderTarget(quickRenderTarget);
    const auto rendererCleanup =
        qScopeGuard([&window, &renderControl, &renderTarget,
                     &renderPassDescriptor, &depthStencil, &colorBuffer] {
            window.setRenderTarget({});
            renderControl.invalidate();
            renderTarget.reset();
            renderPassDescriptor.reset();
            depthStencil.reset();
            colorBuffer.reset();
        });

    QQmlEngine engine;
    if (captureSelection.has_value()) {
        const CaptureSelection selection = *captureSelection;
        BenchmarkUniformProvider provider(physicalSize);
        Scenario scenario;
        if (!scenario.initialize(&engine, &window, &provider, compiled.stages,
                                 selection.renderer, selection.passCount,
                                 logicalSize, physicalSize, &error)) {
            QTextStream(stderr) << "RenderDoc scenario " << selection.name()
                                << " could not initialize: " << error << '\n';
            return 1;
        }

        quint64 captureFrame = 0;
        if (!scenario.render(&renderControl, rhi, colorBuffer.get(),
                             selection.workload, ++captureFrame, nullptr, true,
                             &error)) {
            QTextStream(stderr) << "RenderDoc scenario " << selection.name()
                                << " setup failed: " << error << '\n';
            return 1;
        }
        for (int iteration = 0; iteration < *warmup; ++iteration) {
            if (!scenario.render(&renderControl, rhi, colorBuffer.get(),
                                 selection.workload, ++captureFrame, nullptr,
                                 false, &error)) {
                QTextStream(stderr) << "RenderDoc scenario " << selection.name()
                                    << " warmup failed: " << error << '\n';
                return 1;
            }
        }

        RenderDocCaptureScope captureScope(*renderDocCapture);
        if (!captureScope.started()) {
            QTextStream(stderr) << "unable to start RenderDoc capture: "
                                << renderDocCapture->errorString() << '\n';
            return 4;
        }
        if (!scenario.render(&renderControl, rhi, colorBuffer.get(),
                             selection.workload, ++captureFrame, nullptr, false,
                             &error)) {
            QTextStream(stderr) << "RenderDoc scenario " << selection.name()
                                << " capture frame failed: " << error << '\n';
            return 1;
        }
        if (!captureScope.finish()) {
            QTextStream(stderr) << "unable to finish RenderDoc capture: "
                                << renderDocCapture->errorString() << '\n';
            return 4;
        }

        const QString capturePath = !parser.isSet(renderDocCapturePathOption)
            ? QStringLiteral("configured-by-renderdoc")
            : parser.value(renderDocCapturePathOption);
        QTextStream(stdout)
            << "renderdoc_capture=" << selection.name()
            << " renderdoc_api=" << renderDocCapture->apiVersion()
            << " graphics_api=" << requestedGraphicsApi
            << " rhi_backend=" << rhi->backendName() << " debug_markers="
            << (rhi->isFeatureSupported(QRhi::DebugMarkers) ? "supported"
                                                            : "unavailable")
            << " warmup=" << *warmup << " path_template=" << capturePath
            << '\n';
        return 0;
    }

    QVector<ScenarioResult> results;
    results.reserve(static_cast<qsizetype>(passCounts.size()) * 2 * 2);
    quint64 frame = 0;
    const int measurementRoundCount =
        std::min(maximumMeasurementRounds, *iterations);
    for (const Workload workload :
         {Workload::SourceDirty, Workload::EffectOnly}) {
        for (const int passCount : passCounts) {
            std::array<ScenarioAccumulator, 2> accumulators{{
                {
                    .renderer = Renderer::Legacy,
                    .workload = workload,
                    .passCount = passCount,
                },
                {
                    .renderer = Renderer::Retained,
                    .workload = workload,
                    .passCount = passCount,
                },
            }};
            for (ScenarioAccumulator &accumulator : accumulators) {
                accumulator.cpuRecordSamples.reserve(*iterations);
                accumulator.cpuCompletionSamples.reserve(*iterations);
                accumulator.cpuTotalSamples.reserve(*iterations);
                accumulator.gpuSamples.reserve(*iterations);
            }

            // Measure each comparable pair in AB/BA order. Recreating both
            // scenes in the second round makes scene construction symmetric,
            // while the setup render and distributed warmup stay unmeasured.
            for (int round = 0; round < measurementRoundCount; ++round) {
                const std::array<Renderer, 2> rendererOrder = round % 2 == 0
                    ? std::array{Renderer::Legacy, Renderer::Retained}
                    : std::array{Renderer::Retained, Renderer::Legacy};
                const int measuredFrameCount =
                    framesInRound(*iterations, round, measurementRoundCount);
                const int warmupFrameCount =
                    framesInRound(*warmup, round, measurementRoundCount);
                for (const Renderer renderer : rendererOrder) {
                    ScenarioAccumulator &accumulator =
                        accumulators.at(rendererIndex(renderer));
                    BenchmarkUniformProvider provider(physicalSize);
                    Scenario scenario;
                    if (!scenario.initialize(&engine, &window, &provider,
                                             compiled.stages, renderer,
                                             passCount, logicalSize,
                                             physicalSize, &error)) {
                        QTextStream(stderr)
                            << "renderer=" << rendererName(renderer)
                            << " workload=" << workloadName(workload)
                            << " passes=" << passCount << " round=" << round
                            << ": " << error << '\n';
                        return 1;
                    }

                    if (!scenario.render(&renderControl, rhi, colorBuffer.get(),
                                         workload, ++frame, nullptr, true,
                                         &error)) {
                        QTextStream(stderr)
                            << "renderer=" << rendererName(renderer)
                            << " workload=" << workloadName(workload)
                            << " passes=" << passCount << " round=" << round
                            << " setup: " << error << '\n';
                        return 1;
                    }
                    ++accumulator.validationReadbackCount;
                    for (int iteration = 0; iteration < warmupFrameCount;
                         ++iteration) {
                        if (!scenario.render(&renderControl, rhi,
                                             colorBuffer.get(), workload,
                                             ++frame, nullptr, false, &error)) {
                            QTextStream(stderr)
                                << "renderer=" << rendererName(renderer)
                                << " workload=" << workloadName(workload)
                                << " passes=" << passCount << " round=" << round
                                << " warmup: " << error << '\n';
                            return 1;
                        }
                    }

                    const std::optional<TerminalCustomShaderPipelineSnapshot>
                        before = scenario.pipelineSnapshot();
                    const quint64 paintCountBefore =
                        scenario.sourcePaintCount();
                    for (int iteration = 0; iteration < measuredFrameCount;
                         ++iteration) {
                        FrameTiming timing;
                        if (!scenario.render(&renderControl, rhi,
                                             colorBuffer.get(), workload,
                                             ++frame, &timing, false, &error)) {
                            QTextStream(stderr)
                                << "renderer=" << rendererName(renderer)
                                << " workload=" << workloadName(workload)
                                << " passes=" << passCount << " round=" << round
                                << " iteration=" << iteration << ": " << error
                                << '\n';
                            return 1;
                        }
                        accumulator.cpuRecordSamples.append(
                            timing.cpuRecordNanoseconds);
                        accumulator.cpuCompletionSamples.append(
                            timing.cpuCompletionNanoseconds);
                        accumulator.cpuTotalSamples.append(
                            timing.cpuRecordNanoseconds
                            + timing.cpuCompletionNanoseconds);
                        if (timing.gpuNanoseconds.has_value()) {
                            accumulator.gpuSamples.append(
                                *timing.gpuNanoseconds);
                        }
                    }
                    const quint64 sourcePaintDelta =
                        scenario.sourcePaintCount() - paintCountBefore;
                    const std::optional<TerminalCustomShaderPipelineSnapshot>
                        after = scenario.pipelineSnapshot();
                    const quint64 expectedSourcePaints =
                        workload == Workload::SourceDirty
                        ? static_cast<quint64>(measuredFrameCount)
                        : 0;
                    if (sourcePaintDelta != expectedSourcePaints) {
                        QTextStream(stderr)
                            << "renderer=" << rendererName(renderer)
                            << " workload=" << workloadName(workload)
                            << " passes=" << passCount << " round=" << round
                            << ": source paint delta was " << sourcePaintDelta
                            << ", expected " << expectedSourcePaints << '\n';
                        return 1;
                    }
                    accumulator.sourcePaintDelta += sourcePaintDelta;

                    if (before.has_value() != after.has_value()) {
                        QTextStream(stderr)
                            << "renderer=" << rendererName(renderer)
                            << " workload=" << workloadName(workload)
                            << " passes=" << passCount << " round=" << round
                            << ": retained telemetry disappeared\n";
                        return 1;
                    }
                    if (before.has_value()) {
                        const std::uint64_t expectedFrames =
                            static_cast<std::uint64_t>(measuredFrameCount);
                        const std::uint64_t expectedDraws = expectedFrames
                            * static_cast<std::uint64_t>(passCount);
                        if (counterDelta(after->frameCount, before->frameCount)
                                != expectedFrames
                            || counterDelta(after->drawCount, before->drawCount)
                                != expectedDraws
                            || after->targetCreateCount
                                != before->targetCreateCount
                            || after->targetDestroyCount
                                != before->targetDestroyCount
                            || after->pipelineCreateCount
                                != before->pipelineCreateCount
                            || after->sourceBindingUpdateCount
                                != before->sourceBindingUpdateCount
                            || after->resourceGeneration
                                != before->resourceGeneration) {
                            QTextStream(stderr)
                                << "renderer=" << rendererName(renderer)
                                << " workload=" << workloadName(workload)
                                << " passes=" << passCount << " round=" << round
                                << ": retained steady-state telemetry changed "
                                   "unexpectedly\n";
                            return 1;
                        }
                        accumulator.renderedFrameCount +=
                            counterDelta(after->frameCount, before->frameCount);
                        accumulator.recordedDrawCount +=
                            counterDelta(after->drawCount, before->drawCount);
                        accumulator.targetCreateCount +=
                            counterDelta(after->targetCreateCount,
                                         before->targetCreateCount);
                        accumulator.targetDestroyCount +=
                            counterDelta(after->targetDestroyCount,
                                         before->targetDestroyCount);
                        accumulator.pipelineCreateCount +=
                            counterDelta(after->pipelineCreateCount,
                                         before->pipelineCreateCount);
                        accumulator.sourceBindingUpdateCount +=
                            counterDelta(after->sourceBindingUpdateCount,
                                         before->sourceBindingUpdateCount);
                        accumulator.latestSnapshot = after;
                    }
                }
            }

            for (ScenarioAccumulator &accumulator : accumulators) {
                if (accumulator.cpuTotalSamples.size() != *iterations
                    || accumulator.validationReadbackCount
                        != measurementRoundCount) {
                    QTextStream(stderr)
                        << "renderer=" << rendererName(accumulator.renderer)
                        << " workload=" << workloadName(workload)
                        << " passes=" << passCount
                        << ": measurement accounting mismatch\n";
                    return 1;
                }

                std::optional<TerminalCustomShaderPipelineSnapshot> before;
                std::optional<TerminalCustomShaderPipelineSnapshot> after;
                if (accumulator.latestSnapshot.has_value()) {
                    before = accumulator.latestSnapshot;
                    after = accumulator.latestSnapshot;
                    before->frameCount = 0;
                    before->drawCount = 0;
                    before->targetCreateCount = 0;
                    before->targetDestroyCount = 0;
                    before->pipelineCreateCount = 0;
                    before->sourceBindingUpdateCount = 0;
                    after->frameCount = accumulator.renderedFrameCount;
                    after->drawCount = accumulator.recordedDrawCount;
                    after->targetCreateCount = accumulator.targetCreateCount;
                    after->targetDestroyCount = accumulator.targetDestroyCount;
                    after->pipelineCreateCount =
                        accumulator.pipelineCreateCount;
                    after->sourceBindingUpdateCount =
                        accumulator.sourceBindingUpdateCount;
                }

                const bool hasCompleteGpuTiming =
                    accumulator.gpuSamples.size() == *iterations;
                results.append({
                    .renderer = accumulator.renderer,
                    .workload = workload,
                    .passCount = passCount,
                    .cpuRecordTiming =
                        summarize(std::move(accumulator.cpuRecordSamples)),
                    .cpuCompletionTiming =
                        summarize(std::move(accumulator.cpuCompletionSamples)),
                    .cpuTotalTiming = summarize(accumulator.cpuTotalSamples),
                    .gpuTiming = hasCompleteGpuTiming
                        ? std::optional<Summary>(
                              summarize(accumulator.gpuSamples))
                        : std::nullopt,
                    .validGpuSampleCount =
                        static_cast<int>(accumulator.gpuSamples.size()),
                    .baselineCpuTotalSamples = passCount == 0
                        ? std::move(accumulator.cpuTotalSamples)
                        : QVector<qint64>{},
                    .baselineGpuSamples = passCount == 0 && hasCompleteGpuTiming
                        ? std::move(accumulator.gpuSamples)
                        : QVector<qint64>{},
                    .sourcePaintDelta = accumulator.sourcePaintDelta,
                    .before = before,
                    .after = after,
                });
            }
        }
    }

    const std::optional<WorkloadBaseline> sourceDirtyBaseline =
        sharedBaseline(results, Workload::SourceDirty);
    const std::optional<WorkloadBaseline> effectOnlyBaseline =
        sharedBaseline(results, Workload::EffectOnly);
    if (!sourceDirtyBaseline.has_value() || !effectOnlyBaseline.has_value()) {
        QTextStream(stderr)
            << "unable to construct shared zero-pass baselines\n";
        return 1;
    }

    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(2);
    output << "qt=" << qVersion()
           << " platform=" << QGuiApplication::platformName()
           << " graphics_api=" << requestedGraphicsApi
           << " rhi_backend=" << rhi->backendName() << " gpu_timestamps="
           << (rhi->isFeatureSupported(QRhi::Timestamps) ? "supported"
                                                         : "unavailable")
           << " viewport=" << logicalSize.width() << 'x' << logicalSize.height()
           << " framebuffer=" << physicalSize.width() << 'x'
           << physicalSize.height() << " dpr=" << dpr << " warmup=" << *warmup
           << " iterations=" << *iterations
           << " measurement_rounds=" << measurementRoundCount
           << " renderer_order="
           << (measurementRoundCount == 2 ? "legacy-retained/"
                                            "retained-legacy"
                                          : "legacy-retained")
           << " measured_frames_per_round="
           << framesInRound(*iterations, 0, measurementRoundCount);
    if (measurementRoundCount == 2) {
        output << '/' << framesInRound(*iterations, 1, measurementRoundCount);
    }
    output << " completion=offscreen-end-frame"
           << " validation_readbacks_per_scenario=" << measurementRoundCount
           << " gpu_scope=whole-command-buffer"
           << " gpu_delta_baseline=pooled-workload-pass0"
           << " transforms=ordered-affine\n";

    for (const WorkloadBaseline *const baseline :
         {std::addressof(*sourceDirtyBaseline),
          std::addressof(*effectOnlyBaseline)}) {
        output << "baseline workload=" << workloadName(baseline->workload)
               << " cpu_total_median_us="
               << baseline->cpuTotalTiming.medianMicroseconds
               << " pass0_cpu_renderer_gap_us="
               << baseline->cpuRendererGapMicroseconds
               << " pass0_cpu_renderer_gap_ratio="
               << baseline->cpuRendererGapMicroseconds
                / baseline->cpuTotalTiming.medianMicroseconds
               << " cpu_pooled_samples=" << baseline->pooledCpuSampleCount;
        if (baseline->gpuTiming.has_value()) {
            output << " gpu_median_us="
                   << baseline->gpuTiming->medianMicroseconds
                   << " pass0_gpu_renderer_gap_us="
                   << *baseline->gpuRendererGapMicroseconds
                   << " pass0_gpu_renderer_gap_ratio="
                   << *baseline->gpuRendererGapMicroseconds
                    / baseline->gpuTiming->medianMicroseconds
                   << " gpu_pooled_samples=" << baseline->pooledGpuSampleCount;
        } else {
            output << " gpu_timing=unavailable gpu_pooled_samples=0";
        }
        output << '\n';
    }

    const std::uint64_t bytesPerTarget =
        static_cast<std::uint64_t>(physicalSize.width())
        * static_cast<std::uint64_t>(physicalSize.height()) * 4U;
    for (const ScenarioResult &result : std::as_const(results)) {
        const WorkloadBaseline &baseline =
            result.workload == Workload::SourceDirty ? *sourceDirtyBaseline
                                                     : *effectOnlyBaseline;
        const ScenarioResult *const legacy = findResult(
            results, Renderer::Legacy, result.workload, result.passCount);
        const double baselineMedian =
            baseline.cpuTotalTiming.medianMicroseconds;
        const double legacyMedian =
            legacy != nullptr ? legacy->cpuTotalTiming.medianMicroseconds : 0.0;
        const std::optional<double> gpuBaselineMedian =
            baseline.gpuTiming.has_value()
            ? std::optional<double>(baseline.gpuTiming->medianMicroseconds)
            : std::nullopt;
        const std::optional<double> gpuMedianDelta =
            result.gpuTiming.has_value() && gpuBaselineMedian.has_value()
            ? std::optional<double>(result.gpuTiming->medianMicroseconds
                                    - *gpuBaselineMedian)
            : std::nullopt;
        const int offscreenTargetCount = result.renderer == Renderer::Legacy
            ? result.passCount
            : (result.passCount == 0 ? 0
                                     : 1
                       + (result.after.has_value()
                              ? result.after->liveTargetCount
                              : terminalCustomShaderPipelineTargetCount(
                                    result.passCount)));

        output << "renderer=" << rendererName(result.renderer)
               << " workload=" << workloadName(result.workload)
               << " passes=" << result.passCount << " cpu_record_min_us="
               << result.cpuRecordTiming.minimumMicroseconds
               << " cpu_record_median_us="
               << result.cpuRecordTiming.medianMicroseconds
               << " cpu_record_p90_us="
               << result.cpuRecordTiming.p90Microseconds
               << " cpu_record_mean_us="
               << result.cpuRecordTiming.meanMicroseconds
               << " cpu_record_max_us="
               << result.cpuRecordTiming.maximumMicroseconds
               << " cpu_completion_min_us="
               << result.cpuCompletionTiming.minimumMicroseconds
               << " cpu_completion_median_us="
               << result.cpuCompletionTiming.medianMicroseconds
               << " cpu_completion_p90_us="
               << result.cpuCompletionTiming.p90Microseconds
               << " cpu_completion_mean_us="
               << result.cpuCompletionTiming.meanMicroseconds
               << " cpu_completion_max_us="
               << result.cpuCompletionTiming.maximumMicroseconds
               << " cpu_total_median_us="
               << result.cpuTotalTiming.medianMicroseconds
               << " cpu_total_p90_us=" << result.cpuTotalTiming.p90Microseconds
               << " cpu_total_median_delta_us="
               << result.cpuTotalTiming.medianMicroseconds - baselineMedian
               << " cpu_total_vs_legacy_ratio="
               << (legacyMedian > 0.0
                       ? result.cpuTotalTiming.medianMicroseconds / legacyMedian
                       : 0.0)
               << " estimated_offscreen_targets=" << offscreenTargetCount
               << " estimated_offscreen_bytes="
               << static_cast<std::uint64_t>(offscreenTargetCount)
                * bytesPerTarget
               << " source_paints=" << result.sourcePaintDelta;

        if (result.gpuTiming.has_value()) {
            output << " gpu_min_us=" << result.gpuTiming->minimumMicroseconds
                   << " gpu_median_us=" << result.gpuTiming->medianMicroseconds
                   << " gpu_p90_us=" << result.gpuTiming->p90Microseconds
                   << " gpu_mean_us=" << result.gpuTiming->meanMicroseconds
                   << " gpu_max_us=" << result.gpuTiming->maximumMicroseconds
                   << " gpu_median_delta_us=";
            if (gpuMedianDelta.has_value()) {
                output << *gpuMedianDelta;
            } else {
                output << "na";
            }
            output << " gpu_vs_legacy_ratio=";
            if (legacy != nullptr && legacy->gpuTiming.has_value()
                && legacy->gpuTiming->medianMicroseconds > 0.0) {
                output << result.gpuTiming->medianMicroseconds
                        / legacy->gpuTiming->medianMicroseconds;
            } else {
                output << "na";
            }
            output << " gpu_valid_samples=" << result.validGpuSampleCount;
        } else {
            output << " gpu_timing=unavailable"
                   << " gpu_valid_samples=" << result.validGpuSampleCount;
        }

        if (result.after.has_value()) {
            const TerminalCustomShaderPipelineSnapshot empty;
            const TerminalCustomShaderPipelineSnapshot &before =
                result.before.value_or(empty);
            const TerminalCustomShaderPipelineSnapshot &after = *result.after;
            output << " internal_targets=" << after.liveTargetCount
                   << " internal_texture_bytes=" << after.ownedTextureBytes
                   << " rendered_frames="
                   << counterDelta(after.frameCount, before.frameCount)
                   << " recorded_draws="
                   << counterDelta(after.drawCount, before.drawCount)
                   << " target_creates="
                   << counterDelta(after.targetCreateCount,
                                   before.targetCreateCount)
                   << " target_destroys="
                   << counterDelta(after.targetDestroyCount,
                                   before.targetDestroyCount)
                   << " pipeline_creates="
                   << counterDelta(after.pipelineCreateCount,
                                   before.pipelineCreateCount)
                   << " source_binding_updates="
                   << counterDelta(after.sourceBindingUpdateCount,
                                   before.sourceBindingUpdateCount)
                   << " resource_generation=" << after.resourceGeneration;
        } else {
            output << " internal_targets=na"
                   << " internal_texture_bytes=na"
                   << " rendered_frames=na"
                   << " recorded_draws=na"
                   << " target_creates=na"
                   << " target_destroys=na"
                   << " pipeline_creates=na"
                   << " source_binding_updates=na"
                   << " resource_generation=na";
        }
        output << " measured_frames=" << *iterations
               << " validation_readbacks=" << measurementRoundCount << '\n';
    }
    return 0;
}

#include "bench_terminal_custom_shader_rhi.moc"
