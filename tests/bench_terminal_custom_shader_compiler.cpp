#include "terminal_custom_shader_compiler.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>

namespace {

constexpr auto shaderBody = R"glsl(
void mainImage(out vec4 color, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec4 terminalColor = texture(iChannel0, uv);
    float scanline = 0.98 + 0.02 * sin(fragCoord.y * 3.14159265);
    color = vec4(terminalColor.rgb * scanline, terminalColor.a);
}
)glsl";

struct Summary {
    double minimumMicroseconds = 0.0;
    double medianMicroseconds = 0.0;
    double p90Microseconds = 0.0;
    double meanMicroseconds = 0.0;
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
    const auto micros = [](qint64 nanos) {
        return static_cast<double>(nanos) / 1'000.0;
    };
    return {
        .minimumMicroseconds = micros(samples.constFirst()),
        .medianMicroseconds = micros(samples.at(count / 2)),
        .p90Microseconds = micros(samples.at(p90)),
        .meanMicroseconds = micros(total) / static_cast<double>(samples.size()),
    };
}

bool writeSources(const QDir &directory, int count,
                  TerminalCustomShaderOptions *options)
{
    options->sources.clear();
    for (int index = 0; index < count; ++index) {
        const QString path =
            directory.filePath(QStringLiteral("pass-%1.glsl").arg(index));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        const QByteArray source = QByteArrayLiteral("// pass ")
            + QByteArray::number(index) + QByteArrayLiteral("\n")
            + QByteArray(shaderBody);
        if (file.write(source) != source.size()) {
            return false;
        }
        options->sources.append({.path = path, .optional = false});
    }
    return true;
}

bool measure(int iterations, int passCount, bool cold, QVector<qint64> *samples,
             int *compiled, int *hits, QString *error)
{
    QTemporaryDir sourceRoot;
    if (!sourceRoot.isValid()) {
        *error = QStringLiteral("unable to create source directory");
        return false;
    }
    TerminalCustomShaderOptions options;
    if (!writeSources(QDir(sourceRoot.path()), passCount, &options)) {
        *error = QStringLiteral("unable to write benchmark shaders");
        return false;
    }

    QTemporaryDir warmCache;
    if (!warmCache.isValid()) {
        *error = QStringLiteral("unable to create cache directory");
        return false;
    }
    if (!cold) {
        const auto primed =
            compileTerminalCustomShaders(options, warmCache.path());
        if (!primed.succeeded()) {
            *error = primed.diagnostic;
            return false;
        }
    }

    samples->reserve(iterations);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        std::unique_ptr<QTemporaryDir> coldCache;
        if (cold) {
            coldCache = std::make_unique<QTemporaryDir>();
            if (!coldCache->isValid()) {
                *error =
                    QStringLiteral("unable to create cold cache directory");
                return false;
            }
        }
        const QString cache = cold ? coldCache->path() : warmCache.path();
        QElapsedTimer timer;
        timer.start();
        const TerminalCustomShaderCompileResult result =
            compileTerminalCustomShaders(options, cache);
        samples->append(timer.nsecsElapsed());
        if (!result.succeeded()) {
            *error = result.diagnostic;
            return false;
        }
        *compiled += result.metrics.compiledCount;
        *hits += result.metrics.cacheHitCount;
    }
    return true;
}

void printResult(QTextStream &out, QStringView mode, int passCount,
                 int iterations, const Summary &summary, int compiled, int hits)
{
    out << mode << " passes=" << passCount << " iterations=" << iterations
        << " min_us=" << summary.minimumMicroseconds
        << " median_us=" << summary.medianMicroseconds
        << " p90_us=" << summary.p90Microseconds
        << " mean_us=" << summary.meanMicroseconds << " compiled=" << compiled
        << " cache_hits=" << hits << '\n';
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-custom-shader-compiler"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Ghostty Qt custom-shader compiler microbenchmark"));
    parser.addHelpOption();
    QCommandLineOption coldIterations(
        QStringLiteral("cold-iterations"),
        QStringLiteral("Cold bake iterations per pass count."),
        QStringLiteral("count"), QStringLiteral("5"));
    QCommandLineOption warmIterations(
        QStringLiteral("warm-iterations"),
        QStringLiteral("Warm cache iterations per pass count."),
        QStringLiteral("count"), QStringLiteral("100"));
    parser.addOptions({coldIterations, warmIterations});
    parser.process(application);

    bool coldOk = false;
    bool warmOk = false;
    const int coldCount = parser.value(coldIterations).toInt(&coldOk);
    const int warmCount = parser.value(warmIterations).toInt(&warmOk);
    if (!coldOk || !warmOk || coldCount < 1 || warmCount < 1) {
        QTextStream(stderr) << "iteration counts must be positive integers\n";
        return 2;
    }

    QTextStream out(stdout);
    out.setRealNumberNotation(QTextStream::FixedNotation);
    out.setRealNumberPrecision(2);
    out << "qt=" << qVersion()
        << " prefix=" << terminalCustomShaderCompilerCacheVersion << '\n';

    for (const int passCount : {1, 2, 4, 8}) {
        QVector<qint64> coldSamples;
        QVector<qint64> warmSamples;
        int coldCompiled = 0;
        int coldHits = 0;
        int warmCompiled = 0;
        int warmHits = 0;
        QString error;
        if (!measure(coldCount, passCount, true, &coldSamples, &coldCompiled,
                     &coldHits, &error)
            || !measure(warmCount, passCount, false, &warmSamples,
                        &warmCompiled, &warmHits, &error)) {
            QTextStream(stderr) << error << '\n';
            return 1;
        }
        const Summary cold = summarize(std::move(coldSamples));
        const Summary warm = summarize(std::move(warmSamples));
        printResult(out, u"cold", passCount, coldCount, cold, coldCompiled,
                    coldHits);
        printResult(out, u"warm", passCount, warmCount, warm, warmCompiled,
                    warmHits);
        out << "speedup passes=" << passCount << " median_ratio="
            << cold.medianMicroseconds / warm.medianMicroseconds << '\n';
    }
    return 0;
}
