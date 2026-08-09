#include "session_worker.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

#include <termios.h>
#include <unistd.h>

namespace {

struct Options {
    quint64 bytes = 16ULL * 1024ULL * 1024ULL;
    int chunkBytes = 64;
    int warmup = 1;
    int iterations = 5;
    int timeoutMilliseconds = 30'000;
    bool ansi = false;
};

struct Sample {
    qint64 elapsedNanoseconds = 0;
    qint64 firstFrameNanoseconds = -1;
    qint64 finalFrameNanoseconds = -1;
    TerminalSessionIoMetrics io;
    QVector<qint64> eventLoopGapsNanoseconds;
};

std::optional<quint64> positiveInteger(const QString &value,
                                       bool allowZero = false)
{
    bool ok = false;
    const quint64 result = value.toULongLong(&ok);
    if (!ok || (!allowZero && result == 0)) return std::nullopt;
    return result;
}

QByteArray childPayload(int size, bool ansi)
{
    const QByteArray pattern = ansi
        ? QByteArrayLiteral("\033[31mred\033[0m    ")
        : QByteArrayLiteral("terminal-session-io ");
    QByteArray result;
    result.reserve(size);
    while (result.size() < size) result += pattern;
    result.truncate(size);
    return result;
}

int runChild(quint64 byteCount, int chunkBytes, bool ansi)
{
    struct termios attributes {};
    if (::tcgetattr(STDOUT_FILENO, &attributes) == 0) {
        ::cfmakeraw(&attributes);
        (void)::tcsetattr(STDOUT_FILENO, TCSANOW, &attributes);
    }

    const QByteArray payload = childPayload(chunkBytes, ansi);
    quint64 remaining = byteCount;
    while (remaining > 0) {
        const size_t requested = static_cast<size_t>(
            std::min<quint64>(remaining, static_cast<quint64>(payload.size())));
        ssize_t written = -1;
        do {
            written = ::write(STDOUT_FILENO, payload.constData(), requested);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) return 1;
        remaining -= static_cast<quint64>(written);
    }
    return 0;
}

template <typename Predicate>
bool pumpUntil(Predicate &&predicate, int timeoutMilliseconds)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate()) {
        if (elapsed.elapsed() >= timeoutMilliseconds) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::yieldCurrentThread();
    }
    return true;
}

std::optional<Sample> runSample(const Options &options, bool legacy,
                                QString *error)
{
    if (legacy) {
        qputenv("GHOSTTY_QT_PTY_READ_BATCHING", QByteArrayLiteral("legacy"));
    } else {
        qunsetenv("GHOSTTY_QT_PTY_READ_BATCHING");
    }

    SessionWorker worker;
    bool exited = false;
    int exitCode = -1;
    int signalNumber = -1;
    bool childStarted = false;
    Sample sample;
    QElapsedTimer elapsed;
    elapsed.start();

    QObject::connect(&worker, &SessionWorker::terminalUpdated, &worker,
                     [&](const TerminalUpdate &) {
                         if (!childStarted) return;
                         const qint64 now = elapsed.nsecsElapsed();
                         if (sample.firstFrameNanoseconds < 0) {
                             sample.firstFrameNanoseconds = now;
                         }
                         sample.finalFrameNanoseconds = now;
                     });
    QObject::connect(&worker, &SessionWorker::started, &worker,
                     [&](qint64) { childStarted = true; });
    QObject::connect(&worker, &SessionWorker::errorOccurred, &worker,
                     [&](const QString &message) {
                         if (error->isEmpty()) *error = message;
                     });
    QObject::connect(
        &worker, &SessionWorker::sessionExited, &worker,
        [&](int code, int signal, bool, bool, quint64, bool) {
            exited = true;
            exitCode = code;
            signalNumber = signal;
        });

    QElapsedTimer eventLoopTimer;
    eventLoopTimer.start();
    QTimer latencyProbe;
    latencyProbe.setTimerType(Qt::PreciseTimer);
    latencyProbe.setInterval(1);
    QObject::connect(&latencyProbe, &QTimer::timeout, &worker, [&] {
        sample.eventLoopGapsNanoseconds.append(eventLoopTimer.nsecsElapsed());
        eventLoopTimer.restart();
    });

    TerminalSessionLaunchOptions launch;
    launch.workingDirectory = QCoreApplication::applicationDirPath();
    launch.program = {
        QCoreApplication::applicationFilePath(),
        QStringLiteral("--bench-child"),
        QStringLiteral("--bytes"),
        QString::number(options.bytes),
        QStringLiteral("--chunk-bytes"),
        QString::number(options.chunkBytes),
        QStringLiteral("--corpus"),
        options.ansi ? QStringLiteral("ansi") : QStringLiteral("plain"),
    };
    launch.hold = true;
    launch.scrollbackLimits.bytes = 1ULL * 1024ULL * 1024ULL;
    launch.scrollbackLimits.lines = 1'000;
    launch.runtime.scrollbackCompression = false;
    launch.initialGeometry = TerminalSessionGeometry{
        .columns = 160,
        .rows = 48,
        .cellWidthPixels = 8,
        .cellHeightPixels = 16,
        .surfaceWidthPixels = 1'280,
        .surfaceHeightPixels = 768,
        .padding = {},
    };

    latencyProbe.start();
    if (!worker.initialize(launch)
        || !pumpUntil([&] { return exited || !error->isEmpty(); },
                      options.timeoutMilliseconds)) {
        if (error->isEmpty()) *error = QStringLiteral("session timed out");
        latencyProbe.stop();
        worker.shutdown();
        return std::nullopt;
    }
    latencyProbe.stop();
    sample.elapsedNanoseconds = elapsed.nsecsElapsed();
    sample.io = worker.sessionIoMetrics();
    worker.shutdown();
    if (exitCode != 0 || signalNumber != 0) {
        *error = QStringLiteral("child failed with exit=%1 signal=%2")
                     .arg(exitCode)
                     .arg(signalNumber);
        return std::nullopt;
    }
    if (sample.io.readBytes != options.bytes
        || sample.io.parserBytes != options.bytes) {
        *error = QStringLiteral("transport byte mismatch: read=%1 parsed=%2 expected=%3")
                     .arg(sample.io.readBytes)
                     .arg(sample.io.parserBytes)
                     .arg(options.bytes);
        return std::nullopt;
    }
    return sample;
}

double percentileMilliseconds(QVector<qint64> samples, double percentile)
{
    if (samples.isEmpty()) return 0.0;
    std::ranges::sort(samples);
    const qsizetype index = std::min(
        samples.size() - 1,
        static_cast<qsizetype>(std::ceil(samples.size() * percentile) - 1));
    return static_cast<double>(samples.at(index)) / 1'000'000.0;
}

bool runMode(const Options &options, bool legacy, QTextStream &output)
{
    QVector<Sample> samples;
    for (int iteration = 0;
         iteration < options.warmup + options.iterations; ++iteration) {
        QString error;
        std::optional<Sample> sample = runSample(options, legacy, &error);
        if (!sample) {
            QTextStream(stderr) << error << '\n';
            return false;
        }
        if (iteration >= options.warmup) samples.append(std::move(*sample));
    }

    QVector<qint64> elapsed;
    QVector<qint64> firstFrames;
    QVector<qint64> finalFrames;
    QVector<qint64> eventLoopGaps;
    TerminalSessionIoMetrics metrics;
    for (const Sample &sample : samples) {
        elapsed.append(sample.elapsedNanoseconds);
        firstFrames.append(sample.firstFrameNanoseconds);
        finalFrames.append(sample.finalFrameNanoseconds);
        eventLoopGaps += sample.eventLoopGapsNanoseconds;
        metrics.readActivations += sample.io.readActivations;
        metrics.continuationActivations += sample.io.continuationActivations;
        metrics.readCalls += sample.io.readCalls;
        metrics.readBytes += sample.io.readBytes;
        metrics.readWouldBlock += sample.io.readWouldBlock;
        metrics.parserSubmissions += sample.io.parserSubmissions;
        metrics.parserBytes += sample.io.parserBytes;
        metrics.maximumParserBatchBytes =
            std::max(metrics.maximumParserBatchBytes,
                     sample.io.maximumParserBatchBytes);
    }
    std::ranges::sort(elapsed);
    const qint64 medianElapsed = elapsed.at(elapsed.size() / 2);
    const double mibPerSecond =
        static_cast<double>(options.bytes) * 1'000'000'000.0
        / static_cast<double>(medianElapsed) / (1024.0 * 1024.0);
    output << "mode=" << (legacy ? "legacy" : "batched")
           << " corpus=" << (options.ansi ? "ansi" : "plain")
           << " bytes=" << options.bytes
           << " chunk_bytes=" << options.chunkBytes
           << " median_mib_s=" << mibPerSecond
           << " first_frame_p50_ms="
           << percentileMilliseconds(firstFrames, 0.5)
           << " final_frame_p50_ms="
           << percentileMilliseconds(finalFrames, 0.5)
           << " event_loop_gap_p95_ms="
           << percentileMilliseconds(eventLoopGaps, 0.95)
           << " event_loop_gap_p99_ms="
           << percentileMilliseconds(eventLoopGaps, 0.99)
           << " read_activations=" << metrics.readActivations
           << " continuation_activations=" << metrics.continuationActivations
           << " read_calls=" << metrics.readCalls
           << " read_eagain=" << metrics.readWouldBlock
           << " parser_submissions=" << metrics.parserSubmissions
           << " parser_bytes=" << metrics.parserBytes
           << " maximum_parser_batch_bytes="
           << metrics.maximumParserBatchBytes << '\n';
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-session-io"));
    if (!qEnvironmentVariableIsSet("GHOSTTY_QT_TERMINFO")) {
        qputenv("GHOSTTY_QT_TERMINFO",
                QByteArrayLiteral(GHOSTTY_QT_BENCH_TERMINFO));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("End-to-end PTY session transport benchmark"));
    parser.addHelpOption();
    const QCommandLineOption bytesOption(
        QStringLiteral("bytes"), QStringLiteral("Bytes emitted by the child."),
        QStringLiteral("count"), QStringLiteral("16777216"));
    const QCommandLineOption chunkOption(
        QStringLiteral("chunk-bytes"),
        QStringLiteral("Bytes per child write()."), QStringLiteral("count"),
        QStringLiteral("64"));
    const QCommandLineOption warmupOption(
        QStringLiteral("warmup"), QStringLiteral("Unmeasured iterations."),
        QStringLiteral("count"), QStringLiteral("1"));
    const QCommandLineOption iterationsOption(
        QStringLiteral("iterations"), QStringLiteral("Measured iterations."),
        QStringLiteral("count"), QStringLiteral("5"));
    const QCommandLineOption timeoutOption(
        QStringLiteral("timeout-ms"), QStringLiteral("Per-run timeout."),
        QStringLiteral("milliseconds"), QStringLiteral("30000"));
    const QCommandLineOption corpusOption(
        QStringLiteral("corpus"), QStringLiteral("plain or ansi."),
        QStringLiteral("name"), QStringLiteral("plain"));
    const QCommandLineOption modeOption(
        QStringLiteral("mode"), QStringLiteral("legacy, batched, or both."),
        QStringLiteral("name"), QStringLiteral("both"));
    const QCommandLineOption childOption(QStringLiteral("bench-child"));
    parser.addOptions({bytesOption, chunkOption, warmupOption, iterationsOption,
                       timeoutOption, corpusOption, modeOption, childOption});
    parser.process(application);

    const auto bytes = positiveInteger(parser.value(bytesOption));
    const auto chunk = positiveInteger(parser.value(chunkOption));
    const auto warmup = positiveInteger(parser.value(warmupOption), true);
    const auto iterations = positiveInteger(parser.value(iterationsOption));
    const auto timeout = positiveInteger(parser.value(timeoutOption));
    if (!bytes || !chunk || !warmup || !iterations || !timeout
        || *chunk > 1024ULL * 1024ULL
        || *warmup > static_cast<quint64>(std::numeric_limits<int>::max())
        || *iterations > static_cast<quint64>(std::numeric_limits<int>::max())
        || *timeout > static_cast<quint64>(std::numeric_limits<int>::max())) {
        QTextStream(stderr) << "numeric option is outside its valid range\n";
        return 2;
    }
    const QString corpus = parser.value(corpusOption).trimmed().toLower();
    const QString mode = parser.value(modeOption).trimmed().toLower();
    if ((corpus != QStringLiteral("plain")
         && corpus != QStringLiteral("ansi"))
        || (mode != QStringLiteral("legacy")
            && mode != QStringLiteral("batched")
            && mode != QStringLiteral("both"))) {
        QTextStream(stderr) << "invalid corpus or mode\n";
        return 2;
    }
    if (parser.isSet(childOption)) {
        return runChild(*bytes, static_cast<int>(*chunk),
                        corpus == QStringLiteral("ansi"));
    }

    Options options;
    options.bytes = *bytes;
    options.chunkBytes = static_cast<int>(*chunk);
    options.warmup = static_cast<int>(*warmup);
    options.iterations = static_cast<int>(*iterations);
    options.timeoutMilliseconds = static_cast<int>(*timeout);
    options.ansi = corpus == QStringLiteral("ansi");
    QTextStream output(stdout);
    output << "benchmark_contract=1 qt=" << qVersion() << '\n';
    if ((mode == QStringLiteral("legacy") || mode == QStringLiteral("both"))
        && !runMode(options, true, output)) {
        return 1;
    }
    if ((mode == QStringLiteral("batched") || mode == QStringLiteral("both"))
        && !runMode(options, false, output)) {
        return 1;
    }
    return 0;
}
