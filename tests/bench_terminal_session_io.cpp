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
#include <array>
#include <cerrno>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

#include <termios.h>
#include <unistd.h>

namespace {

enum class Workload {
    Output,
    Replies,
};

struct Options {
    Workload workload = Workload::Output;
    quint64 bytes = 16ULL * 1024ULL * 1024ULL;
    int chunkBytes = 64;
    quint64 queries = 10'000;
    int responseBytes = 64;
    int queryBurst = 1;
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

bool writeAll(int descriptor, QByteArrayView bytes)
{
    while (!bytes.isEmpty()) {
        ssize_t written = -1;
        do {
            written = ::write(descriptor, bytes.data(),
                              static_cast<size_t>(bytes.size()));
        } while (written < 0 && errno == EINTR);
        if (written <= 0) return false;
        bytes = bytes.sliced(static_cast<qsizetype>(written));
    }
    return true;
}

QByteArray replyPayload(int size)
{
    constexpr QByteArrayView pattern("ghostty-query-reply-");
    QByteArray result(size, Qt::Uninitialized);
    for (qsizetype index = 0; index < result.size(); ++index) {
        result[index] = pattern.at(index % pattern.size());
    }
    return result;
}

void makeRawPty()
{
    struct termios attributes{};
    if (::tcgetattr(STDOUT_FILENO, &attributes) == 0) {
        ::cfmakeraw(&attributes);
        (void)::tcsetattr(STDOUT_FILENO, TCSANOW, &attributes);
    }
}

int runOutputChild(quint64 byteCount, int chunkBytes, bool ansi)
{
    makeRawPty();

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

int runReplyChild(quint64 queryCount, int queryBurst, int responseBytes)
{
    makeRawPty();

    const QByteArray expected = replyPayload(responseBytes);
    const QByteArray queries(queryBurst, '\x05');
    std::array<char, 64 * 1024> buffer{};
    quint64 remainingQueries = queryCount;
    while (remainingQueries > 0) {
        const int burst = static_cast<int>(std::min<quint64>(
            remainingQueries, static_cast<quint64>(queryBurst)));
        if (!writeAll(STDOUT_FILENO, QByteArrayView(queries).first(burst))) {
            return 1;
        }

        const quint64 expectedByteCount =
            static_cast<quint64>(burst) * static_cast<quint64>(responseBytes);
        quint64 received = 0;
        while (received < expectedByteCount) {
            const size_t requested = static_cast<size_t>(
                std::min<quint64>(expectedByteCount - received,
                                  static_cast<quint64>(buffer.size())));
            ssize_t count = -1;
            do {
                count = ::read(STDIN_FILENO, buffer.data(), requested);
            } while (count < 0 && errno == EINTR);
            if (count <= 0) return 1;
            for (ssize_t index = 0; index < count; ++index) {
                const quint64 responseOffset =
                    (received + static_cast<quint64>(index))
                    % static_cast<quint64>(expected.size());
                if (buffer.at(static_cast<size_t>(index))
                    != expected.at(static_cast<qsizetype>(responseOffset))) {
                    return 1;
                }
            }
            received += static_cast<quint64>(count);
        }
        remainingQueries -= static_cast<quint64>(burst);
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

std::optional<Sample> runSample(const Options &options, bool legacyReads,
                                bool legacyWrites, QString *error)
{
    if (legacyReads) {
        qputenv("GHOSTTY_QT_PTY_READ_BATCHING", QByteArrayLiteral("legacy"));
    } else {
        qunsetenv("GHOSTTY_QT_PTY_READ_BATCHING");
    }
    if (legacyWrites) {
        qputenv("GHOSTTY_QT_PTY_WRITE_DIRECT", QByteArrayLiteral("legacy"));
    } else {
        qunsetenv("GHOSTTY_QT_PTY_WRITE_DIRECT");
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
    QObject::connect(&worker, &SessionWorker::sessionExited, &worker,
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
        QStringLiteral("--workload"),
        options.workload == Workload::Output ? QStringLiteral("output")
                                             : QStringLiteral("replies"),
        QStringLiteral("--bytes"),
        QString::number(options.bytes),
        QStringLiteral("--chunk-bytes"),
        QString::number(options.chunkBytes),
        QStringLiteral("--queries"),
        QString::number(options.queries),
        QStringLiteral("--response-bytes"),
        QString::number(options.responseBytes),
        QStringLiteral("--query-burst"),
        QString::number(options.queryBurst),
        QStringLiteral("--corpus"),
        options.ansi ? QStringLiteral("ansi") : QStringLiteral("plain")};
    launch.hold = true;
    launch.scrollbackLimits.bytes = 1ULL * 1024ULL * 1024ULL;
    launch.scrollbackLimits.lines = 1'000;
    launch.runtime.scrollbackCompression = false;
    if (options.workload == Workload::Replies) {
        launch.runtime.enquiryResponse = replyPayload(options.responseBytes);
    }
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
    const quint64 expectedReadBytes =
        options.workload == Workload::Output ? options.bytes : options.queries;
    if (sample.io.readBytes != expectedReadBytes
        || sample.io.parserBytes != expectedReadBytes) {
        *error = QStringLiteral(
                     "transport byte mismatch: read=%1 parsed=%2 expected=%3")
                     .arg(sample.io.readBytes)
                     .arg(sample.io.parserBytes)
                     .arg(expectedReadBytes);
        return std::nullopt;
    }
    if (options.workload == Workload::Replies) {
        const quint64 expectedWriteBytes =
            options.queries * static_cast<quint64>(options.responseBytes);
        if (sample.io.writeSubmissions != options.queries
            || sample.io.writeSubmissionBytes != expectedWriteBytes
            || sample.io.writeBytes != expectedWriteBytes) {
            *error = QStringLiteral(
                         "reply mismatch: submissions=%1 submitted=%2 "
                         "written=%3 expected_submissions=%4 expected_bytes=%5")
                         .arg(sample.io.writeSubmissions)
                         .arg(sample.io.writeSubmissionBytes)
                         .arg(sample.io.writeBytes)
                         .arg(options.queries)
                         .arg(expectedWriteBytes);
            return std::nullopt;
        }
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

bool runMode(const Options &options, bool legacyReads, bool legacyWrites,
             QTextStream &output)
{
    QVector<Sample> samples;
    for (int iteration = 0; iteration < options.warmup + options.iterations;
         ++iteration) {
        QString error;
        std::optional<Sample> sample =
            runSample(options, legacyReads, legacyWrites, &error);
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
        if (sample.firstFrameNanoseconds >= 0) {
            firstFrames.append(sample.firstFrameNanoseconds);
        }
        if (sample.finalFrameNanoseconds >= 0) {
            finalFrames.append(sample.finalFrameNanoseconds);
        }
        eventLoopGaps += sample.eventLoopGapsNanoseconds;
        metrics.readActivations += sample.io.readActivations;
        metrics.continuationActivations += sample.io.continuationActivations;
        metrics.readCalls += sample.io.readCalls;
        metrics.readBytes += sample.io.readBytes;
        metrics.readWouldBlock += sample.io.readWouldBlock;
        metrics.parserSubmissions += sample.io.parserSubmissions;
        metrics.parserBytes += sample.io.parserBytes;
        metrics.maximumParserBatchBytes = std::max(
            metrics.maximumParserBatchBytes, sample.io.maximumParserBatchBytes);
        metrics.writeSubmissions += sample.io.writeSubmissions;
        metrics.writeSubmissionBytes += sample.io.writeSubmissionBytes;
        metrics.writeCalls += sample.io.writeCalls;
        metrics.writeBytes += sample.io.writeBytes;
        metrics.writeWouldBlock += sample.io.writeWouldBlock;
        metrics.directWriteBytes += sample.io.directWriteBytes;
        metrics.writeBufferedCopyBytes += sample.io.writeBufferedCopyBytes;
        metrics.writeBufferAllocations += sample.io.writeBufferAllocations;
    }
    std::ranges::sort(elapsed);
    const qint64 medianElapsed = elapsed.at(elapsed.size() / 2);
    const quint64 workloadBytes = options.workload == Workload::Output
        ? options.bytes
        : options.queries * static_cast<quint64>(options.responseBytes);
    const double mibPerSecond = static_cast<double>(workloadBytes)
        * 1'000'000'000.0 / static_cast<double>(medianElapsed)
        / (1024.0 * 1024.0);
    output << "workload="
           << (options.workload == Workload::Output ? "output" : "replies");
    if (options.workload == Workload::Output) {
        output << " mode=" << (legacyReads ? "legacy" : "batched")
               << " corpus=" << (options.ansi ? "ansi" : "plain")
               << " bytes=" << options.bytes
               << " chunk_bytes=" << options.chunkBytes;
    } else {
        output << " write_mode=" << (legacyWrites ? "legacy" : "direct")
               << " queries=" << options.queries
               << " response_bytes=" << options.responseBytes
               << " query_burst=" << options.queryBurst << " query_mean_us="
               << static_cast<double>(medianElapsed)
                / static_cast<double>(options.queries) / 1'000.0;
    }
    output << " median_mib_s=" << mibPerSecond
           << " first_frame_p50_ms=" << percentileMilliseconds(firstFrames, 0.5)
           << " final_frame_p50_ms=" << percentileMilliseconds(finalFrames, 0.5)
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
           << " maximum_parser_batch_bytes=" << metrics.maximumParserBatchBytes
           << " write_submissions=" << metrics.writeSubmissions
           << " write_submission_bytes=" << metrics.writeSubmissionBytes
           << " write_calls=" << metrics.writeCalls
           << " write_bytes=" << metrics.writeBytes
           << " write_eagain=" << metrics.writeWouldBlock
           << " direct_write_bytes=" << metrics.directWriteBytes
           << " write_buffered_copy_bytes=" << metrics.writeBufferedCopyBytes
           << " write_buffer_allocations=" << metrics.writeBufferAllocations
           << '\n';
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
    const QCommandLineOption workloadOption(
        QStringLiteral("workload"), QStringLiteral("output or replies."),
        QStringLiteral("name"), QStringLiteral("output"));
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
    const QCommandLineOption queriesOption(
        QStringLiteral("queries"),
        QStringLiteral("ENQ round trips for the replies workload."),
        QStringLiteral("count"), QStringLiteral("10000"));
    const QCommandLineOption responseBytesOption(
        QStringLiteral("response-bytes"),
        QStringLiteral("Bytes returned for each ENQ."), QStringLiteral("count"),
        QStringLiteral("64"));
    const QCommandLineOption queryBurstOption(
        QStringLiteral("query-burst"),
        QStringLiteral("ENQs emitted before reading their ordered replies."),
        QStringLiteral("count"), QStringLiteral("1"));
    const QCommandLineOption writeModeOption(
        QStringLiteral("write-mode"),
        QStringLiteral("legacy, direct, or both for the replies workload."),
        QStringLiteral("name"), QStringLiteral("both"));
    const QCommandLineOption childOption(QStringLiteral("bench-child"));
    parser.addOptions({workloadOption, bytesOption, chunkOption, queriesOption,
                       responseBytesOption, queryBurstOption, warmupOption,
                       iterationsOption, timeoutOption, corpusOption,
                       modeOption, writeModeOption, childOption});
    parser.process(application);

    const auto bytes = positiveInteger(parser.value(bytesOption));
    const auto chunk = positiveInteger(parser.value(chunkOption));
    const auto warmup = positiveInteger(parser.value(warmupOption), true);
    const auto iterations = positiveInteger(parser.value(iterationsOption));
    const auto timeout = positiveInteger(parser.value(timeoutOption));
    const auto queries = positiveInteger(parser.value(queriesOption));
    const auto responseBytes =
        positiveInteger(parser.value(responseBytesOption));
    const auto queryBurst = positiveInteger(parser.value(queryBurstOption));
    if (!bytes || !chunk || !queries || !responseBytes || !queryBurst || !warmup
        || !iterations || !timeout || *chunk > 1024ULL * 1024ULL
        || *responseBytes > 1024ULL * 1024ULL || *queryBurst > 64ULL * 1024ULL
        || *responseBytes * *queryBurst > 64ULL * 1024ULL * 1024ULL
        || *warmup > static_cast<quint64>(std::numeric_limits<int>::max())
        || *iterations > static_cast<quint64>(std::numeric_limits<int>::max())
        || *timeout > static_cast<quint64>(std::numeric_limits<int>::max())) {
        QTextStream(stderr) << "numeric option is outside its valid range\n";
        return 2;
    }
    const QString workload = parser.value(workloadOption).trimmed().toLower();
    const QString corpus = parser.value(corpusOption).trimmed().toLower();
    const QString mode = parser.value(modeOption).trimmed().toLower();
    const QString writeMode = parser.value(writeModeOption).trimmed().toLower();
    if ((workload != QStringLiteral("output")
         && workload != QStringLiteral("replies"))
        || (corpus != QStringLiteral("plain")
            && corpus != QStringLiteral("ansi"))
        || (mode != QStringLiteral("legacy")
            && mode != QStringLiteral("batched")
            && mode != QStringLiteral("both"))
        || (writeMode != QStringLiteral("legacy")
            && writeMode != QStringLiteral("direct")
            && writeMode != QStringLiteral("both"))) {
        QTextStream(stderr) << "invalid workload, corpus, or mode\n";
        return 2;
    }
    if (parser.isSet(childOption)) {
        return workload == QStringLiteral("output")
            ? runOutputChild(*bytes, static_cast<int>(*chunk),
                             corpus == QStringLiteral("ansi"))
            : runReplyChild(*queries, static_cast<int>(*queryBurst),
                            static_cast<int>(*responseBytes));
    }

    Options options;
    options.workload = workload == QStringLiteral("output") ? Workload::Output
                                                            : Workload::Replies;
    options.bytes = *bytes;
    options.chunkBytes = static_cast<int>(*chunk);
    options.queries = *queries;
    options.responseBytes = static_cast<int>(*responseBytes);
    options.queryBurst = static_cast<int>(*queryBurst);
    options.warmup = static_cast<int>(*warmup);
    options.iterations = static_cast<int>(*iterations);
    options.timeoutMilliseconds = static_cast<int>(*timeout);
    options.ansi = corpus == QStringLiteral("ansi");
    QTextStream output(stdout);
    output << "benchmark_contract=1 qt=" << qVersion() << '\n';
    if (options.workload == Workload::Output) {
        if ((mode == QStringLiteral("legacy") || mode == QStringLiteral("both"))
            && !runMode(options, true, false, output)) {
            return 1;
        }
        if ((mode == QStringLiteral("batched")
             || mode == QStringLiteral("both"))
            && !runMode(options, false, false, output)) {
            return 1;
        }
    } else {
        if ((writeMode == QStringLiteral("legacy")
             || writeMode == QStringLiteral("both"))
            && !runMode(options, false, true, output)) {
            return 1;
        }
        if ((writeMode == QStringLiteral("direct")
             || writeMode == QStringLiteral("both"))
            && !runMode(options, false, false, output)) {
            return 1;
        }
    }
    return 0;
}
