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
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleRectNode>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

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
    return {
        .minimumMicroseconds = microseconds(samples.constFirst()),
        .medianMicroseconds = microseconds(samples.at(count / 2)),
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

    bool render(QQuickWindow *window, Workload workload, quint64 frame,
                qint64 *elapsed, QString *error)
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

        QElapsedTimer timer;
        if (elapsed != nullptr) timer.start();
        const QImage image = window->grabWindow();
        if (elapsed != nullptr) *elapsed = timer.nsecsElapsed();
        if (image.isNull()) {
            *error = QStringLiteral("QQuickWindow::grabWindow returned null");
            return false;
        }
        if (source_->paintedFrame() != expectedSourceFrame) {
            *error = QStringLiteral(
                         "source frame %1 was not rendered (last painted %2)")
                         .arg(expectedSourceFrame)
                         .arg(source_->paintedFrame());
            return false;
        }
        if (!validatePixel(image, expectedSourceFrame, expectedEffectFrame,
                           error)) {
            return false;
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

struct ScenarioResult {
    Renderer renderer = Renderer::Legacy;
    Workload workload = Workload::SourceDirty;
    int passCount = 0;
    Summary timing;
    quint64 sourcePaintDelta = 0;
    std::optional<TerminalCustomShaderPipelineSnapshot> before;
    std::optional<TerminalCustomShaderPipelineSnapshot> after;
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

std::uint64_t counterDelta(std::uint64_t after, std::uint64_t before)
{
    return after >= before ? after - before : 0;
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

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")) {
        qputenv("QT_SCALE_FACTOR", "1");
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-custom-shader-rhi"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Ghostty Qt retained-versus-legacy custom-shader OpenGL RHI "
        "microbenchmark"));
    parser.addHelpOption();
    const QCommandLineOption warmupOption(
        QStringLiteral("warmup"),
        QStringLiteral("Untimed warm-up frames per scenario."),
        QStringLiteral("count"), QStringLiteral("20"));
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
    parser.addOptions(
        {warmupOption, iterationsOption, widthOption, heightOption});
    parser.process(application);

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
    QString error;
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
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(logicalSize);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window, 5'000)) {
        QTextStream(stderr) << "window did not become exposed on platform "
                            << QGuiApplication::platformName() << '\n';
        return 1;
    }
    const qreal dpr = window.devicePixelRatio();
    const QSize physicalSize(qMax(1, qRound(logicalSize.width() * dpr)),
                             qMax(1, qRound(logicalSize.height() * dpr)));

    QQmlEngine engine;
    QVector<ScenarioResult> results;
    results.reserve(static_cast<qsizetype>(passCounts.size()) * 2 * 2);
    quint64 frame = 0;
    bool graphicsApiChecked = false;
    for (const Workload workload :
         {Workload::SourceDirty, Workload::EffectOnly}) {
        for (const int passCount : passCounts) {
            for (const Renderer renderer :
                 {Renderer::Legacy, Renderer::Retained}) {
                BenchmarkUniformProvider provider(physicalSize);
                Scenario scenario;
                if (!scenario.initialize(&engine, &window, &provider,
                                         compiled.stages, renderer, passCount,
                                         logicalSize, physicalSize, &error)) {
                    QTextStream(stderr)
                        << "renderer=" << rendererName(renderer)
                        << " workload=" << workloadName(workload)
                        << " passes=" << passCount << ": " << error << '\n';
                    return 1;
                }

                if (!scenario.render(&window, workload, ++frame, nullptr,
                                     &error)) {
                    QTextStream(stderr)
                        << "renderer=" << rendererName(renderer)
                        << " workload=" << workloadName(workload)
                        << " passes=" << passCount << ": " << error << '\n';
                    return 1;
                }
                if (!graphicsApiChecked) {
                    graphicsApiChecked = true;
                    if (window.rendererInterface()->graphicsApi()
                        != QSGRendererInterface::OpenGL) {
                        QTextStream(stderr)
                            << "OpenGL RHI was requested, but Qt selected "
                               "graphics API "
                            << static_cast<int>(
                                   window.rendererInterface()->graphicsApi())
                            << '\n';
                        return 3;
                    }
                }
                for (int iteration = 0; iteration < *warmup; ++iteration) {
                    if (!scenario.render(&window, workload, ++frame, nullptr,
                                         &error)) {
                        QTextStream(stderr)
                            << "renderer=" << rendererName(renderer)
                            << " workload=" << workloadName(workload)
                            << " passes=" << passCount << " warmup: " << error
                            << '\n';
                        return 1;
                    }
                }

                const std::optional<TerminalCustomShaderPipelineSnapshot>
                    before = scenario.pipelineSnapshot();
                const quint64 paintCountBefore = scenario.sourcePaintCount();
                QVector<qint64> samples;
                samples.reserve(*iterations);
                for (int iteration = 0; iteration < *iterations; ++iteration) {
                    qint64 elapsed = 0;
                    if (!scenario.render(&window, workload, ++frame, &elapsed,
                                         &error)) {
                        QTextStream(stderr)
                            << "renderer=" << rendererName(renderer)
                            << " workload=" << workloadName(workload)
                            << " passes=" << passCount
                            << " iteration=" << iteration << ": " << error
                            << '\n';
                        return 1;
                    }
                    samples.append(elapsed);
                }
                const quint64 sourcePaintDelta =
                    scenario.sourcePaintCount() - paintCountBefore;
                if ((workload == Workload::SourceDirty
                     && sourcePaintDelta != static_cast<quint64>(*iterations))
                    || (workload == Workload::EffectOnly
                        && sourcePaintDelta != 0)) {
                    QTextStream(stderr)
                        << "renderer=" << rendererName(renderer)
                        << " workload=" << workloadName(workload)
                        << " passes=" << passCount
                        << ": source paint delta was " << sourcePaintDelta
                        << ", expected "
                        << (workload == Workload::SourceDirty ? *iterations : 0)
                        << '\n';
                    return 1;
                }
                results.append({
                    .renderer = renderer,
                    .workload = workload,
                    .passCount = passCount,
                    .timing = summarize(std::move(samples)),
                    .sourcePaintDelta = sourcePaintDelta,
                    .before = before,
                    .after = scenario.pipelineSnapshot(),
                });
            }
        }
    }

    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(2);
    output << "qt=" << qVersion()
           << " platform=" << QGuiApplication::platformName()
           << " graphics_api=opengl-rhi"
           << " viewport=" << logicalSize.width() << 'x' << logicalSize.height()
           << " framebuffer=" << physicalSize.width() << 'x'
           << physicalSize.height() << " dpr=" << dpr << " warmup=" << *warmup
           << " iterations=" << *iterations
           << " completion=grab-readback transforms=ordered-affine\n";

    const std::uint64_t bytesPerTarget =
        static_cast<std::uint64_t>(physicalSize.width())
        * static_cast<std::uint64_t>(physicalSize.height()) * 4U;
    for (const ScenarioResult &result : std::as_const(results)) {
        const ScenarioResult *const baseline =
            findResult(results, result.renderer, result.workload, 0);
        const ScenarioResult *const legacy = findResult(
            results, Renderer::Legacy, result.workload, result.passCount);
        const double baselineMedian =
            baseline != nullptr ? baseline->timing.medianMicroseconds : 0.0;
        const double legacyMedian =
            legacy != nullptr ? legacy->timing.medianMicroseconds : 0.0;
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
               << " passes=" << result.passCount
               << " min_us=" << result.timing.minimumMicroseconds
               << " median_us=" << result.timing.medianMicroseconds
               << " p90_us=" << result.timing.p90Microseconds
               << " mean_us=" << result.timing.meanMicroseconds
               << " max_us=" << result.timing.maximumMicroseconds
               << " median_delta_us="
               << result.timing.medianMicroseconds - baselineMedian
               << " retained_vs_legacy_ratio="
               << (legacyMedian > 0.0
                       ? result.timing.medianMicroseconds / legacyMedian
                       : 0.0)
               << " estimated_offscreen_targets=" << offscreenTargetCount
               << " estimated_offscreen_bytes="
               << static_cast<std::uint64_t>(offscreenTargetCount)
                * bytesPerTarget
               << " source_paints=" << result.sourcePaintDelta;

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
        output << " verified_frames=" << *iterations << '\n';
    }
    return 0;
}

#include "bench_terminal_custom_shader_rhi.moc"
