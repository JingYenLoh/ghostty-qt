#include "terminal_custom_shader_compiler.h"
#include "terminal_custom_shader_qsg.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
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
#include <limits>
#include <memory>
#include <numeric>
#include <optional>

namespace {

constexpr int maximumPassCount = 4;

struct PassTransform {
    double scale = 1.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

constexpr std::array<PassTransform, maximumPassCount> passTransforms{{
    {.scale = 0.75, .red = 0.0625},
    {.scale = 0.80, .green = 0.0625},
    {.scale = 0.85, .blue = 0.0625},
    {.scale = 0.90, .red = 0.125},
}};

constexpr std::array<QByteArrayView, maximumPassCount> shaderTransforms{{
    "terminalColor.rgb * 0.75 + vec3(0.0625, 0.0, 0.0)",
    "terminalColor.rgb * 0.80 + vec3(0.0, 0.0625, 0.0)",
    "terminalColor.rgb * 0.85 + vec3(0.0, 0.0, 0.0625)",
    "terminalColor.rgb * 0.90 + vec3(0.125, 0.0, 0.0)",
}};

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

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override
    {
        auto *node = static_cast<QSGSimpleRectNode *>(oldNode);
        if (node == nullptr) node = new QSGSimpleRectNode;
        node->setRect(boundingRect());
        node->setColor(color_);
        paintedFrame_.store(frame_, std::memory_order_release);
        return node;
    }

private:
    QColor color_ = sourceColorForFrame(0);
    quint64 frame_ = 0;
    std::atomic<quint64> paintedFrame_{std::numeric_limits<quint64>::max()};
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
    {
        auto uniforms = std::make_shared<TerminalCustomShaderUniforms>();
        uniforms->resolution = {
            static_cast<float>(physicalSize.width()),
            static_cast<float>(physicalSize.height()),
            1.0F,
        };
        uniforms_ = std::move(uniforms);
    }

    [[nodiscard]] TerminalCustomShaderUniformSnapshot
    terminalCustomShaderUniformSnapshot(int) const override
    {
        return uniforms_;
    }

private:
    TerminalCustomShaderUniformSnapshot uniforms_;
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
        const QByteArray source = QByteArrayLiteral(R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec4 terminalColor = texture(iChannel0, uv);
    color = vec4()glsl")
            + QByteArray(shaderTransforms.at(static_cast<std::size_t>(index)))
            + QByteArrayLiteral(", terminalColor.a);\n"
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

QString benchmarkQml(int passCount)
{
    QString qml = QStringLiteral("import QtQuick\n"
                                 "import GhosttyShaderBench 1.0\n"
                                 "Item {\n"
                                 "  width: benchWidth\n"
                                 "  height: benchHeight\n");
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
                    int passCount, QSize logicalSize, QSize physicalSize,
                    QString *error)
    {
        passCount_ = passCount;
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
        for (int stage = 0; stage < passCount; ++stage) {
            context_->setContextProperty(
                QStringLiteral("shaderPath%1").arg(stage),
                stages.at(stage).qsbPath);
            context_->setContextProperty(
                QStringLiteral("shaderData%1").arg(stage),
                stages.at(stage).serializedShader);
        }

        QQmlComponent component(engine);
        const QByteArray qml = benchmarkQml(passCount).toUtf8();
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
        root_->setParentItem(window->contentItem());
        return true;
    }

    bool render(QQuickWindow *window, quint64 frame, qint64 *elapsed,
                QString *error)
    {
        QElapsedTimer timer;
        if (elapsed != nullptr) timer.start();
        source_->setFrame(frame);
        const QImage image = window->grabWindow();
        if (elapsed != nullptr) *elapsed = timer.nsecsElapsed();
        if (image.isNull()) {
            *error = QStringLiteral("QQuickWindow::grabWindow returned null");
            return false;
        }
        if (source_->paintedFrame() != frame) {
            *error = QStringLiteral(
                         "source frame %1 was not rendered (last painted %2)")
                         .arg(frame)
                         .arg(source_->paintedFrame());
            return false;
        }
        return validatePixel(image, frame, error);
    }

private:
    bool validatePixel(const QImage &image, quint64 frame, QString *error) const
    {
        const QColor source = sourceColorForFrame(frame);
        std::array<double, 3> transformed{{
            static_cast<double>(source.red()) / 255.0,
            static_cast<double>(source.green()) / 255.0,
            static_cast<double>(source.blue()) / 255.0,
        }};
        for (int stage = 0; stage < passCount_; ++stage) {
            const PassTransform transform =
                passTransforms.at(static_cast<std::size_t>(stage));
            transformed.at(0) =
                transformed.at(0) * transform.scale + transform.red;
            transformed.at(1) =
                transformed.at(1) * transform.scale + transform.green;
            transformed.at(2) =
                transformed.at(2) * transform.scale + transform.blue;
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
        constexpr int tolerance = 4;
        for (std::size_t channel = 0; channel < expected.size(); ++channel) {
            if (std::abs(observed.at(channel) - expected.at(channel))
                > tolerance) {
                *error = QStringLiteral("pass validation failed for frame %1: "
                                        "expected rgba=(%2,%3,%4,%5), got "
                                        "(%6,%7,%8,%9)")
                             .arg(frame)
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

    int passCount_ = 0;
    std::unique_ptr<QQmlContext> context_;
    std::unique_ptr<QQuickItem> root_;
    BenchmarkSourceItem *source_ = nullptr;
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
        "Ghostty Qt custom-shader OpenGL RHI frame microbenchmark"));
    parser.addHelpOption();
    const QCommandLineOption warmupOption(
        QStringLiteral("warmup"),
        QStringLiteral("Untimed warm-up frames per pass count."),
        QStringLiteral("count"), QStringLiteral("20"));
    const QCommandLineOption iterationsOption(
        QStringLiteral("iterations"),
        QStringLiteral("Measured frames per pass count."),
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
    BenchmarkUniformProvider provider(physicalSize);
    QVector<Summary> summaries;
    summaries.reserve(4);
    quint64 frame = 0;
    for (const int passCount : {0, 1, 2, 4}) {
        Scenario scenario;
        if (!scenario.initialize(&engine, &window, &provider, compiled.stages,
                                 passCount, logicalSize, physicalSize,
                                 &error)) {
            QTextStream(stderr)
                << "passes=" << passCount << ": " << error << '\n';
            return 1;
        }

        if (!scenario.render(&window, ++frame, nullptr, &error)) {
            QTextStream(stderr)
                << "passes=" << passCount << ": " << error << '\n';
            return 1;
        }
        if (window.rendererInterface()->graphicsApi()
            != QSGRendererInterface::OpenGL) {
            QTextStream(stderr)
                << "OpenGL RHI was requested, but Qt selected graphics API "
                << static_cast<int>(window.rendererInterface()->graphicsApi())
                << '\n';
            return 3;
        }
        for (int iteration = 0; iteration < *warmup; ++iteration) {
            if (!scenario.render(&window, ++frame, nullptr, &error)) {
                QTextStream(stderr)
                    << "passes=" << passCount << " warmup: " << error << '\n';
                return 1;
            }
        }

        QVector<qint64> samples;
        samples.reserve(*iterations);
        for (int iteration = 0; iteration < *iterations; ++iteration) {
            qint64 elapsed = 0;
            if (!scenario.render(&window, ++frame, &elapsed, &error)) {
                QTextStream(stderr)
                    << "passes=" << passCount << " iteration=" << iteration
                    << ": " << error << '\n';
                return 1;
            }
            samples.append(elapsed);
        }
        summaries.append(summarize(std::move(samples)));
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
           << " iterations=" << *iterations << " completion=grab-readback\n";
    const Summary baseline = summaries.constFirst();
    int resultIndex = 0;
    for (const int passCount : {0, 1, 2, 4}) {
        const Summary result = summaries.at(resultIndex++);
        const double delta =
            result.medianMicroseconds - baseline.medianMicroseconds;
        output << "passes=" << passCount
               << " min_us=" << result.minimumMicroseconds
               << " median_us=" << result.medianMicroseconds
               << " p90_us=" << result.p90Microseconds
               << " mean_us=" << result.meanMicroseconds
               << " max_us=" << result.maximumMicroseconds
               << " median_delta_us=" << delta << " median_ratio="
               << result.medianMicroseconds / baseline.medianMicroseconds
               << " verified_frames=" << *iterations << '\n';
    }
    return 0;
}

#include "bench_terminal_custom_shader_rhi.moc"
