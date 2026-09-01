#include "session/session_worker.h"
#include "terminal/adapter/ghostty_vt_adapter.h"

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <expected>
#include <limits>
#include <numeric>
#include <optional>

namespace {

constexpr QByteArrayView kNeedle = "search-needle";

struct BenchmarkOptions {
    int fixtureRows = 25'000;
    int columns = 96;
    int viewportRows = 32;
    int markerStride = 8;
    int warmup = 1;
    int iterations = 5;
    int timeoutMilliseconds = 30'000;
    bool compression = true;
};

struct TimingSummary {
    double minimumMicroseconds = 0.0;
    double medianMicroseconds = 0.0;
    double percentile90Microseconds = 0.0;
    double meanMicroseconds = 0.0;
    qint64 totalNanoseconds = 0;
};

struct SearchMeasurement {
    qint64 firstVisibleNanoseconds = -1;
    qint64 completionNanoseconds = -1;
    quint64 canonicalRowsAtFirstVisible = 0;
    quint64 canonicalMatchesAtFirstVisible = 0;
    bool firstVisibleWasComplete = false;
    TerminalSearchUpdate completed;
};

struct CompressionMeasurement {
    qint64 nanoseconds = 0;
    int passes = 0;
    bool wasScheduled = false;
};

struct CancellationMeasurement {
    qint64 firstVisibleNanoseconds = -1;
    qint64 dispatchNanoseconds = -1;
    bool preemptedIncompleteSearch = false;
    int staleUpdates = 0;
    CompressionMeasurement recompression;
};

struct MutationMeasurement {
    qint64 initialFirstVisibleNanoseconds = -1;
    qint64 replacementFirstVisibleNanoseconds = -1;
    qint64 replacementCompletionNanoseconds = -1;
    int staleRevisionUpdates = 0;
    CompressionMeasurement recompression;
};

std::optional<int> integerOption(const QString &value, bool allowZero = false)
{
    bool ok = false;
    const int result = value.toInt(&ok);
    if (!ok || result < (allowZero ? 0 : 1)) return std::nullopt;
    return result;
}

TimingSummary summarize(QVector<qint64> samples)
{
    const qint64 total =
        std::accumulate(samples.cbegin(), samples.cend(), qint64{0});
    std::ranges::sort(samples);
    const qsizetype count = samples.size();
    const qsizetype percentile90Index =
        std::min(count - 1,
                 static_cast<qsizetype>(
                     std::ceil(static_cast<double>(count) * 0.9) - 1.0));
    const auto microseconds = [](qint64 nanoseconds) {
        return static_cast<double>(nanoseconds) / 1'000.0;
    };
    return {
        .minimumMicroseconds = microseconds(samples.constFirst()),
        .medianMicroseconds = microseconds(samples.at(count / 2)),
        .percentile90Microseconds = microseconds(samples.at(percentile90Index)),
        .meanMicroseconds = microseconds(total) / static_cast<double>(count),
        .totalNanoseconds = total,
    };
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

void pumpFor(int milliseconds)
{
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::yieldCurrentThread();
    } while (elapsed.elapsed() < milliseconds);
}

std::expected<QString, QString> writeFixture(const QString &directory,
                                             const BenchmarkOptions &options)
{
    const QString path = directory + QStringLiteral("/search-fixture.txt");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return std::unexpected(
            QStringLiteral("unable to create benchmark fixture: %1")
                .arg(file.errorString()));
    }

    for (int row = 0; row < options.fixtureRows; ++row) {
        QByteArray line = QByteArrayLiteral("row-")
            + QByteArray::number(row).rightJustified(8, '0');
        line += row % options.markerStride == 0
            ? QByteArrayLiteral(" marker-search-needle deterministic-data\n")
            : QByteArrayLiteral(" ordinary-deterministic-data\n");
        if (file.write(line) != line.size()) {
            return std::unexpected(
                QStringLiteral("unable to write benchmark fixture: %1")
                    .arg(file.errorString()));
        }
    }
    if (!file.flush()) {
        return std::unexpected(
            QStringLiteral("unable to flush benchmark fixture: %1")
                .arg(file.errorString()));
    }
    file.close();
    return path;
}

std::expected<CompressionMeasurement, QString>
drainCompression(SessionWorker &worker, QTimer *timer, bool enabled)
{
    if (!enabled) return CompressionMeasurement{};
    if (timer == nullptr) {
        return std::unexpected(
            QStringLiteral("scrollback compression timer was not created"));
    }

    CompressionMeasurement measurement;
    measurement.wasScheduled = timer->isActive();
    timer->stop();
    QElapsedTimer elapsed;
    elapsed.start();
    constexpr int maximumPasses = 100'000;
    for (; measurement.passes < maximumPasses; ++measurement.passes) {
        if (!QMetaObject::invokeMethod(&worker, "compressScrollback",
                                       Qt::DirectConnection)) {
            return std::unexpected(
                QStringLiteral("unable to invoke compression traversal"));
        }
        if (!timer->isActive()) {
            ++measurement.passes;
            measurement.nanoseconds = elapsed.nsecsElapsed();
            return measurement;
        }
        timer->stop();
    }
    return std::unexpected(
        QStringLiteral("compression traversal exceeded %1 passes")
            .arg(maximumPasses));
}

std::expected<SearchMeasurement, QString>
measureCompleteSearch(SessionWorker &worker, quint64 generation,
                      quint64 expectedMatches, int timeoutMilliseconds)
{
    SearchMeasurement measurement;
    QElapsedTimer elapsed;
    const QMetaObject::Connection connection = QObject::connect(
        &worker, &SessionWorker::searchUpdated, &worker,
        [&](const TerminalSearchUpdate &update) {
            if (update.generation != generation) return;
            if (measurement.firstVisibleNanoseconds < 0 && update.active
                && update.visibleCellMask.count(true) > 0) {
                measurement.firstVisibleNanoseconds = elapsed.nsecsElapsed();
                measurement.canonicalRowsAtFirstVisible = update.scannedRows;
                measurement.canonicalMatchesAtFirstVisible =
                    update.totalMatches;
                measurement.firstVisibleWasComplete = update.complete;
            }
            if (update.complete) {
                measurement.completionNanoseconds = elapsed.nsecsElapsed();
                measurement.completed = update;
            }
        });

    elapsed.start();
    worker.search(generation, kNeedle.toByteArray());
    const bool completed =
        pumpUntil([&] { return measurement.completionNanoseconds >= 0; },
                  timeoutMilliseconds);
    QObject::disconnect(connection);
    if (!completed) {
        return std::unexpected(QStringLiteral("search timed out"));
    }
    if (measurement.firstVisibleNanoseconds < 0) {
        return std::unexpected(
            QStringLiteral("search completed without a visible highlight"));
    }
    if (!measurement.completed.active || !measurement.completed.complete
        || measurement.completed.scannedRows != measurement.completed.totalRows
        || measurement.completed.totalMatches != expectedMatches
        || measurement.completed.visibleCellMask.count(true) == 0) {
        return std::unexpected(
            QStringLiteral("completed search failed its result invariants"));
    }
    return measurement;
}

std::expected<CancellationMeasurement, QString>
measureCancellation(SessionWorker &worker, quint64 searchGeneration,
                    quint64 cancelGeneration, QTimer *compressionTimer,
                    const BenchmarkOptions &options)
{
    CancellationMeasurement measurement;
    QElapsedTimer searchElapsed;
    std::optional<TerminalSearchUpdate> cancellationUpdate;
    bool cancellationIssued = false;
    const QMetaObject::Connection connection = QObject::connect(
        &worker, &SessionWorker::searchUpdated, &worker,
        [&](const TerminalSearchUpdate &update) {
            if (update.generation == searchGeneration) {
                if (cancellationIssued) {
                    ++measurement.staleUpdates;
                } else if (measurement.firstVisibleNanoseconds < 0
                           && update.active
                           && update.visibleCellMask.count(true) > 0) {
                    measurement.firstVisibleNanoseconds =
                        searchElapsed.nsecsElapsed();
                    measurement.preemptedIncompleteSearch = !update.complete;
                }
            } else if (update.generation == cancelGeneration) {
                cancellationUpdate = update;
            }
        });

    searchElapsed.start();
    worker.search(searchGeneration, kNeedle.toByteArray());
    const bool highlighted =
        pumpUntil([&] { return measurement.firstVisibleNanoseconds >= 0; },
                  options.timeoutMilliseconds);
    if (!highlighted) {
        QObject::disconnect(connection);
        return std::unexpected(
            QStringLiteral("cancellation search did not highlight viewport"));
    }

    cancellationIssued = true;
    QElapsedTimer dispatch;
    dispatch.start();
    worker.cancelSearch(cancelGeneration);
    measurement.dispatchNanoseconds = dispatch.nsecsElapsed();
    pumpFor(10);
    QObject::disconnect(connection);

    if (!cancellationUpdate.has_value() || cancellationUpdate->active
        || !cancellationUpdate->complete
        || !cancellationUpdate->visibleCellMask.isEmpty()
        || measurement.staleUpdates != 0) {
        return std::unexpected(
            QStringLiteral("cancellation published stale search state"));
    }
    auto recompression =
        drainCompression(worker, compressionTimer, options.compression);
    if (!recompression) return std::unexpected(recompression.error());
    measurement.recompression = *recompression;
    return measurement;
}

std::expected<MutationMeasurement, QString>
measureMutation(SessionWorker &worker, quint64 generation,
                quint64 expectedMatches, QTimer *compressionTimer,
                const BenchmarkOptions &options)
{
    MutationMeasurement measurement;
    QElapsedTimer initialElapsed;
    QElapsedTimer replacementElapsed;
    std::optional<TerminalSearchUpdate> firstVisible;
    std::optional<TerminalSearchUpdate> replacementComplete;
    quint64 originalRevision = 0;
    bool mutationIssued = false;
    const QMetaObject::Connection connection = QObject::connect(
        &worker, &SessionWorker::searchUpdated, &worker,
        [&](const TerminalSearchUpdate &update) {
            if (update.generation != generation) return;
            if (!mutationIssued) {
                if (!firstVisible.has_value() && update.active
                    && update.visibleCellMask.count(true) > 0) {
                    firstVisible = update;
                    originalRevision = update.contentRevision;
                    measurement.initialFirstVisibleNanoseconds =
                        initialElapsed.nsecsElapsed();
                }
                return;
            }
            if (update.contentRevision == originalRevision) {
                ++measurement.staleRevisionUpdates;
                return;
            }
            if (measurement.replacementFirstVisibleNanoseconds < 0
                && update.active && update.visibleCellMask.count(true) > 0) {
                measurement.replacementFirstVisibleNanoseconds =
                    replacementElapsed.nsecsElapsed();
            }
            if (update.complete) {
                measurement.replacementCompletionNanoseconds =
                    replacementElapsed.nsecsElapsed();
                replacementComplete = update;
            }
        });

    initialElapsed.start();
    worker.search(generation, kNeedle.toByteArray());
    if (!pumpUntil([&] { return firstVisible.has_value(); },
                   options.timeoutMilliseconds)) {
        QObject::disconnect(connection);
        return std::unexpected(
            QStringLiteral("mutation search did not highlight viewport"));
    }

    mutationIssued = true;
    replacementElapsed.start();
    const int replacementColumns = options.columns + 1;
    worker.resizeTerminal(replacementColumns, options.viewportRows, 8, 16,
                          replacementColumns * 8, options.viewportRows * 16);
    const bool completed = pumpUntil(
        [&] { return measurement.replacementCompletionNanoseconds >= 0; },
        options.timeoutMilliseconds);
    QObject::disconnect(connection);

    if (!completed || measurement.replacementFirstVisibleNanoseconds < 0
        || !replacementComplete.has_value()
        || replacementComplete->columns != replacementColumns
        || replacementComplete->scannedRows != replacementComplete->totalRows
        || replacementComplete->totalMatches != expectedMatches
        || replacementComplete->visibleCellMask.count(true) == 0
        || measurement.staleRevisionUpdates != 0) {
        return std::unexpected(
            QStringLiteral("mutated search failed its result invariants"));
    }
    auto recompression =
        drainCompression(worker, compressionTimer, options.compression);
    if (!recompression) return std::unexpected(recompression.error());
    measurement.recompression = *recompression;
    return measurement;
}

void printTiming(QTextStream &output, QStringView name,
                 const TimingSummary &summary)
{
    output << name << "_min_us=" << summary.minimumMicroseconds << ' ' << name
           << "_median_us=" << summary.medianMicroseconds << ' ' << name
           << "_p90_us=" << summary.percentile90Microseconds << ' ' << name
           << "_mean_us=" << summary.meanMicroseconds;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-search"));
    if (!qEnvironmentVariableIsSet("GHOSTTY_QT_TERMINFO")) {
        qputenv("GHOSTTY_QT_TERMINFO",
                QByteArrayLiteral(GHOSTTY_QT_BENCH_TERMINFO));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Large-scrollback terminal search and recovery microbenchmark"));
    parser.addHelpOption();
    const QCommandLineOption rowsOption(
        QStringLiteral("rows"), QStringLiteral("Fixture physical rows."),
        QStringLiteral("count"), QStringLiteral("25000"));
    const QCommandLineOption columnsOption(
        QStringLiteral("columns"), QStringLiteral("Terminal columns."),
        QStringLiteral("count"), QStringLiteral("96"));
    const QCommandLineOption viewportRowsOption(
        QStringLiteral("viewport-rows"),
        QStringLiteral("Terminal viewport rows."), QStringLiteral("count"),
        QStringLiteral("32"));
    const QCommandLineOption markerStrideOption(
        QStringLiteral("marker-stride"),
        QStringLiteral("Rows between deterministic matches."),
        QStringLiteral("count"), QStringLiteral("8"));
    const QCommandLineOption warmupOption(
        QStringLiteral("warmup"), QStringLiteral("Unmeasured full searches."),
        QStringLiteral("count"), QStringLiteral("1"));
    const QCommandLineOption iterationsOption(
        QStringLiteral("iterations"), QStringLiteral("Measured full searches."),
        QStringLiteral("count"), QStringLiteral("5"));
    const QCommandLineOption timeoutOption(
        QStringLiteral("timeout-ms"),
        QStringLiteral("Per-operation timeout in milliseconds."),
        QStringLiteral("milliseconds"), QStringLiteral("30000"));
    const QCommandLineOption residentOption(
        QStringLiteral("resident"),
        QStringLiteral("Keep scrollback resident instead of compressing it."));
    parser.addOptions({rowsOption, columnsOption, viewportRowsOption,
                       markerStrideOption, warmupOption, iterationsOption,
                       timeoutOption, residentOption});
    parser.process(application);

    BenchmarkOptions options;
    const std::optional<int> fixtureRows =
        integerOption(parser.value(rowsOption));
    const std::optional<int> columns =
        integerOption(parser.value(columnsOption));
    const std::optional<int> viewportRows =
        integerOption(parser.value(viewportRowsOption));
    const std::optional<int> markerStride =
        integerOption(parser.value(markerStrideOption));
    const std::optional<int> warmup =
        integerOption(parser.value(warmupOption), true);
    const std::optional<int> iterations =
        integerOption(parser.value(iterationsOption));
    const std::optional<int> timeout =
        integerOption(parser.value(timeoutOption));
    if (!fixtureRows || !columns || !viewportRows || !markerStride || !warmup
        || !iterations || !timeout) {
        QTextStream(stderr)
            << "numeric options are outside their valid range\n";
        return 2;
    }
    options.fixtureRows = *fixtureRows;
    options.columns = *columns;
    options.viewportRows = *viewportRows;
    options.markerStride = *markerStride;
    options.warmup = *warmup;
    options.iterations = *iterations;
    options.timeoutMilliseconds = *timeout;
    options.compression = !parser.isSet(residentOption);
    if (options.fixtureRows < options.viewportRows * 8 || options.columns < 48
        || options.markerStride > options.viewportRows) {
        QTextStream(stderr)
            << "rows must cover at least eight viewports, columns must be at "
               "least 48, and marker-stride must not exceed viewport-rows\n";
        return 2;
    }
    if (!QFileInfo::exists(QStringLiteral("/bin/cat"))) {
        QTextStream(stderr) << "/bin/cat is required for the fixture\n";
        return 1;
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        QTextStream(stderr) << "unable to create temporary directory\n";
        return 1;
    }
    const auto fixture = writeFixture(temporary.path(), options);
    if (!fixture) {
        QTextStream(stderr) << fixture.error() << '\n';
        return 1;
    }

    SessionWorker worker;
    TerminalFrame frame;
    QString workerError;
    bool childExited = false;
    int childExitCode = -1;
    int childSignal = -1;
    QObject::connect(&worker, &SessionWorker::terminalUpdated, &application,
                     [&](const TerminalUpdate &update) {
                         if (!applyTerminalUpdate(frame, update)
                             && workerError.isEmpty()) {
                             workerError = QStringLiteral(
                                 "worker published an invalid terminal update");
                         }
                     });
    QObject::connect(&worker, &SessionWorker::errorOccurred, &application,
                     [&](const QString &error) {
                         if (workerError.isEmpty()) workerError = error;
                     });
    QObject::connect(
        &worker, &SessionWorker::sessionExited, &application,
        [&](int exitCode, int signalNumber, bool, bool, quint64, bool) {
            childExited = true;
            childExitCode = exitCode;
            childSignal = signalNumber;
        });

    TerminalSessionLaunchOptions launch;
    launch.workingDirectory = temporary.path();
    launch.program = {QStringLiteral("/bin/cat"), *fixture};
    launch.hold = true;
    launch.initialGeometry = TerminalSessionGeometry{
        .columns = options.columns,
        .rows = options.viewportRows,
        .cellWidthPixels = 8,
        .cellHeightPixels = 16,
        .surfaceWidthPixels = options.columns * 8,
        .surfaceHeightPixels = options.viewportRows * 16,
        .padding = {},
    };
    launch.scrollbackLimits.bytes = 64ULL * 1024ULL * 1024ULL;
    launch.scrollbackLimits.lines =
        static_cast<quint64>(options.fixtureRows + options.viewportRows + 16);
    launch.runtime.scrollbackCompression = options.compression;
    if (!worker.initialize(launch)
        || !pumpUntil([&] { return childExited || !workerError.isEmpty(); },
                      options.timeoutMilliseconds)
        || !workerError.isEmpty() || childExitCode != 0 || childSignal != 0
        || !pumpUntil(
            [&] {
                return frame.scrollTotal
                    >= static_cast<quint64>(options.fixtureRows);
            },
            options.timeoutMilliseconds)) {
        QTextStream(stderr)
            << "fixture terminal failed: "
            << (workerError.isEmpty() ? QStringLiteral("timeout or child error")
                                      : workerError)
            << " exit_code=" << childExitCode << " signal=" << childSignal
            << " scroll_total=" << frame.scrollTotal << '\n';
        worker.shutdown();
        return 1;
    }

    QTimer *compressionTimer =
        worker.findChild<QTimer *>(QStringLiteral("scrollbackCompressionTimer"),
                                   Qt::FindDirectChildrenOnly);
    auto initialCompression =
        drainCompression(worker, compressionTimer, options.compression);
    if (!initialCompression) {
        QTextStream(stderr) << initialCompression.error() << '\n';
        worker.shutdown();
        return 1;
    }

    const quint64 maximumOffset = frame.scrollTotal > frame.scrollLength
        ? frame.scrollTotal - frame.scrollLength
        : 0;
    const quint64 targetOffset = std::min(frame.scrollTotal / 4, maximumOffset);
    const quint64 expectedMatches = static_cast<quint64>(
        (options.fixtureRows - 1) / options.markerStride + 1);
    quint64 generation = 0;
    const auto clearSearch = [&] {
        worker.cancelSearch(++generation);
        pumpFor(2);
    };
    const auto scrollToTarget = [&]() -> bool {
        worker.scrollViewport({
            .kind = TerminalViewportRequest::Kind::Row,
            .row = targetOffset,
        });
        return pumpUntil([&] { return frame.scrollOffset == targetOffset; },
                         options.timeoutMilliseconds);
    };

    QVector<qint64> firstVisibleSamples;
    QVector<qint64> completionSamples;
    QVector<qint64> recompressionSamples;
    qint64 recompressionPasses = 0;
    quint64 canonicalRows = 0;
    quint64 firstVisibleCanonicalRows = 0;
    quint64 firstVisibleCanonicalMatches = 0;
    int firstVisibleCompleteSamples = 0;
    quint64 checksum = 0;
    const int totalIterations = options.warmup + options.iterations;
    for (int iteration = 0; iteration < totalIterations; ++iteration) {
        clearSearch();
        auto baseline =
            drainCompression(worker, compressionTimer, options.compression);
        if (!baseline || !scrollToTarget()) {
            QTextStream(stderr)
                << (baseline
                        ? QStringLiteral("unable to scroll benchmark viewport")
                        : baseline.error())
                << '\n';
            worker.shutdown();
            return 1;
        }
        auto search = measureCompleteSearch(
            worker, ++generation, expectedMatches, options.timeoutMilliseconds);
        if (!search) {
            QTextStream(stderr) << search.error() << '\n';
            worker.shutdown();
            return 1;
        }
        auto recovery =
            drainCompression(worker, compressionTimer, options.compression);
        if (!recovery) {
            QTextStream(stderr) << recovery.error() << '\n';
            worker.shutdown();
            return 1;
        }
        checksum += search->completed.totalMatches
            + search->completed.scannedRows
            + search->canonicalRowsAtFirstVisible
            + search->canonicalMatchesAtFirstVisible
            + static_cast<quint64>(recovery->passes);
        if (iteration >= options.warmup) {
            firstVisibleSamples.append(search->firstVisibleNanoseconds);
            completionSamples.append(search->completionNanoseconds);
            recompressionSamples.append(recovery->nanoseconds);
            recompressionPasses += recovery->passes;
            canonicalRows += search->completed.totalRows;
            firstVisibleCanonicalRows += search->canonicalRowsAtFirstVisible;
            firstVisibleCanonicalMatches +=
                search->canonicalMatchesAtFirstVisible;
            firstVisibleCompleteSamples += search->firstVisibleWasComplete;
        }
    }

    clearSearch();
    auto cancellationBaseline =
        drainCompression(worker, compressionTimer, options.compression);
    if (!cancellationBaseline || !scrollToTarget()) {
        QTextStream(stderr) << "unable to prepare cancellation scenario\n";
        worker.shutdown();
        return 1;
    }
    const quint64 cancellationSearchGeneration = ++generation;
    const quint64 cancellationGeneration = ++generation;
    auto cancellation =
        measureCancellation(worker, cancellationSearchGeneration,
                            cancellationGeneration, compressionTimer, options);
    if (!cancellation) {
        QTextStream(stderr) << cancellation.error() << '\n';
        worker.shutdown();
        return 1;
    }
    checksum += static_cast<quint64>(cancellation->recompression.passes)
        + static_cast<quint64>(cancellation->staleUpdates);

    auto mutationBaseline =
        drainCompression(worker, compressionTimer, options.compression);
    if (!mutationBaseline || !scrollToTarget()) {
        QTextStream(stderr) << "unable to prepare mutation scenario\n";
        worker.shutdown();
        return 1;
    }
    auto mutation = measureMutation(worker, ++generation, expectedMatches,
                                    compressionTimer, options);
    if (!mutation) {
        QTextStream(stderr) << mutation.error() << '\n';
        worker.shutdown();
        return 1;
    }
    checksum += static_cast<quint64>(mutation->recompression.passes)
        + static_cast<quint64>(mutation->staleRevisionUpdates);

    const TimingSummary firstVisibleTiming =
        summarize(std::move(firstVisibleSamples));
    const TimingSummary completionTiming =
        summarize(std::move(completionSamples));
    const TimingSummary recompressionTiming =
        summarize(std::move(recompressionSamples));
    const double completionSeconds =
        static_cast<double>(completionTiming.totalNanoseconds)
        / 1'000'000'000.0;
    QTextStream output(stdout);
    output << "fixture_rows=" << options.fixtureRows
           << " columns=" << options.columns
           << " viewport_rows=" << options.viewportRows
           << " marker_stride=" << options.markerStride
           << " expected_matches=" << expectedMatches
           << " scroll_total=" << frame.scrollTotal
           << " target_offset=" << targetOffset
           << " compression=" << (options.compression ? "enabled" : "resident")
           << " iterations=" << options.iterations << '\n';
    output << "scenario=native_search ";
    printTiming(output, u"first_visible", firstVisibleTiming);
    output << ' ';
    printTiming(output, u"completion", completionTiming);
    output << " canonical_rows_per_second="
           << static_cast<double>(canonicalRows) / completionSeconds
           << " mean_canonical_rows_at_first_visible="
           << static_cast<double>(firstVisibleCanonicalRows)
            / static_cast<double>(options.iterations)
           << " mean_canonical_matches_at_first_visible="
           << static_cast<double>(firstVisibleCanonicalMatches)
            / static_cast<double>(options.iterations)
           << " first_visible_complete_samples=" << firstVisibleCompleteSamples
           << '\n';
    output << "scenario=recompression ";
    printTiming(output, u"drain", recompressionTiming);
    output << " mean_passes="
           << static_cast<double>(recompressionPasses)
            / static_cast<double>(options.iterations)
           << '\n';
    output << "scenario=cancellation first_visible_us="
           << static_cast<double>(cancellation->firstVisibleNanoseconds)
            / 1'000.0
           << " dispatch_us="
           << static_cast<double>(cancellation->dispatchNanoseconds) / 1'000.0
           << " preempted_incomplete="
           << (cancellation->preemptedIncompleteSearch ? "true" : "false")
           << " stale_updates=" << cancellation->staleUpdates
           << " recompress_us="
           << static_cast<double>(cancellation->recompression.nanoseconds)
            / 1'000.0
           << " recompress_passes=" << cancellation->recompression.passes
           << '\n';
    output << "scenario=mutation initial_first_visible_us="
           << static_cast<double>(mutation->initialFirstVisibleNanoseconds)
            / 1'000.0
           << " replacement_first_visible_us="
           << static_cast<double>(mutation->replacementFirstVisibleNanoseconds)
            / 1'000.0
           << " replacement_completion_us="
           << static_cast<double>(mutation->replacementCompletionNanoseconds)
            / 1'000.0
           << " stale_revision_updates=" << mutation->staleRevisionUpdates
           << " recompress_us="
           << static_cast<double>(mutation->recompression.nanoseconds) / 1'000.0
           << " recompress_passes=" << mutation->recompression.passes << '\n';
    output << "checksum=" << checksum << '\n';
    output.flush();

    worker.shutdown();
    return 0;
}
