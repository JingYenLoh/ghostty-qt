#include "ghostty_vt_adapter.h"

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <QTextStream>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>

namespace {

constexpr quint64 maximumBenchmarkCells = 1'000'000;

enum class Workload {
    FullFrame,
    DirtyRows,
};

struct BenchmarkOptions {
    int columns = 160;
    int rows = 48;
    int dirtyRows = 4;
    int warmup = 10;
    int iterations = 100;
};

struct TimingSummary {
    double minimumMicroseconds = 0.0;
    double medianMicroseconds = 0.0;
    double percentile90Microseconds = 0.0;
    double meanMicroseconds = 0.0;
    qint64 totalNanoseconds = 0;
};

struct Measurement {
    QVector<qint64> snapshotNanoseconds;
    QVector<qint64> applyNanoseconds;
    QVector<qint64> endToEndNanoseconds;
    quint64 snapshotChecksum = 1'469'598'103'934'665'603ULL;
    quint64 applyChecksum = 1'469'598'103'934'665'603ULL;
};

std::optional<int> integerOption(const QString &value, bool allowZero = false)
{
    bool ok = false;
    const int result = value.toInt(&ok);
    if (!ok || result < (allowZero ? 0 : 1)) return std::nullopt;
    return result;
}

char cellCharacter(int variant, int row, int column)
{
    constexpr quint64 printableAsciiCount = 94;
    const quint64 value = static_cast<quint64>(variant) * 29ULL
        + static_cast<quint64>(row) * 17ULL
        + static_cast<quint64>(column) * 13ULL;
    return static_cast<char>('!' + value % printableAsciiCount);
}

void appendCursorPosition(QByteArray &bytes, int row, int column)
{
    bytes += QByteArrayLiteral("\033[");
    bytes += QByteArray::number(row + 1);
    bytes += ';';
    bytes += QByteArray::number(column + 1);
    bytes += 'H';
}

void appendRowStyle(QByteArray &bytes, int row)
{
    switch (row % 4) {
    case 0: bytes += QByteArrayLiteral("\033[0m"); break;
    case 1: bytes += QByteArrayLiteral("\033[0;1;31;44m"); break;
    case 2: bytes += QByteArrayLiteral("\033[0;3;4;38;5;202m"); break;
    case 3: bytes += QByteArrayLiteral("\033[0;2;7;9;53;48;2;20;40;60m"); break;
    }
}

void appendRow(QByteArray &bytes, int columns, int row, int variant)
{
    appendCursorPosition(bytes, row, 0);
    appendRowStyle(bytes, row);
    for (int column = 0; column < columns; ++column) {
        bytes += cellCharacter(variant, row, column);
    }
    // Clear pending autowrap without mutating the following row. This keeps
    // the partial workload's dirty-row set exactly equal to its target set.
    bytes += '\r';
}

QByteArray fullFrameCommands(const BenchmarkOptions &options, int variant)
{
    QByteArray bytes;
    bytes.reserve(options.rows * (options.columns + 40));
    bytes += QByteArrayLiteral("\033[?25l");
    for (int row = 0; row < options.rows; ++row) {
        appendRow(bytes, options.columns, row, variant);
    }
    return bytes;
}

QVector<int> dirtyRowIndices(const BenchmarkOptions &options)
{
    QVector<int> result;
    result.reserve(options.dirtyRows);
    if (options.dirtyRows == 1) {
        result.append(options.rows - 1);
        return result;
    }
    for (int index = 0; index < options.dirtyRows; ++index) {
        const qint64 numerator =
            static_cast<qint64>(index) * static_cast<qint64>(options.rows - 1);
        result.append(static_cast<int>(
            numerator / static_cast<qint64>(options.dirtyRows - 1)));
    }
    return result;
}

QByteArray dirtyRowCommands(const BenchmarkOptions &options,
                            const QVector<int> &dirtyRows, int variant)
{
    QByteArray bytes;
    bytes.reserve(dirtyRows.size() * (options.columns + 40));
    for (const int row : dirtyRows) {
        appendRow(bytes, options.columns, row, variant);
    }
    return bytes;
}

quint64 mixChecksum(quint64 checksum, quint64 value)
{
    checksum ^= value;
    checksum *= 1'099'511'628'211ULL;
    return checksum;
}

quint64 terminalCellChecksum(const TerminalCell &cell)
{
    quint64 checksum = 1'469'598'103'934'665'603ULL;
    const std::array flags{
        cell.plainCodepoint(),
        cell.extendedGrapheme(),
        cell.bold(),
        cell.italic(),
        cell.faint(),
        cell.textBlink(),
        cell.inverse(),
        cell.invisible(),
        cell.underlineUsesForeground(),
        cell.strikeThrough(),
        cell.overline(),
        cell.selected(),
        cell.backgroundExplicit(),
        cell.minimumContrastExemptGlyph(),
        cell.hasHyperlink(),
        cell.spacer(),
    };
    checksum = mixChecksum(checksum, cell.baseCodepoint);
    for (const bool flag : flags) {
        checksum = mixChecksum(checksum, flag ? 1 : 0);
    }
    checksum = mixChecksum(checksum,
                           static_cast<quint64>(cell.styleForegroundSource()));
    checksum = mixChecksum(
        checksum, static_cast<quint64>(cell.styleForegroundPaletteIndex() + 1));
    checksum =
        mixChecksum(checksum, static_cast<quint64>(cell.underlineStyle()));
    checksum = mixChecksum(checksum, static_cast<quint64>(cell.columnSpan()));
    checksum = mixChecksum(checksum, cell.foreground.rgba());
    checksum = mixChecksum(checksum, cell.background.rgba());
    return mixChecksum(checksum, cell.underlineColor.rgba());
}

bool validateAndChecksum(const TerminalUpdate &update,
                         const TerminalFrame &frame,
                         const BenchmarkOptions &options, Workload workload,
                         const QVector<int> &dirtyRows, int variant,
                         quint64 *snapshotChecksum, quint64 *applyChecksum,
                         QString *error)
{
    const bool fullFrame = workload == Workload::FullFrame;
    const int expectedRows = fullFrame ? options.rows : options.dirtyRows;
    if (update.columns != options.columns || update.rows != options.rows
        || update.fullFrame != fullFrame
        || update.dirtyRows.size() != expectedRows
        || frame.columns != options.columns || frame.rows != options.rows
        || frame.cells.size()
            != static_cast<qsizetype>(options.columns) * options.rows) {
        QStringList publishedRows;
        publishedRows.reserve(update.dirtyRows.size());
        for (const TerminalRowUpdate &row : update.dirtyRows) {
            publishedRows.append(QString::number(row.row));
        }
        *error =
            QStringLiteral("published update shape did not match workload: "
                           "full=%1 rows=%2 [%3]")
                .arg(update.fullFrame)
                .arg(update.dirtyRows.size())
                .arg(publishedRows.join(','));
        return false;
    }

    for (int index = 0; index < expectedRows; ++index) {
        const int expectedRow = fullFrame ? index : dirtyRows.at(index);
        const TerminalRowUpdate &rowUpdate = update.dirtyRows.at(index);
        if (rowUpdate.row != expectedRow
            || rowUpdate.cells.size() != options.columns) {
            *error =
                QStringLiteral("published dirty-row payload was unexpected");
            return false;
        }
        for (int column = 0; column < options.columns; ++column) {
            const ushort expected =
                static_cast<uchar>(cellCharacter(variant, expectedRow, column));
            const TerminalCell &snapshotCell = rowUpdate.cells.at(column);
            const TerminalCell &appliedCell = frame.cells.at(
                static_cast<qsizetype>(expectedRow) * options.columns + column);
            const quint64 snapshotCellChecksum =
                terminalCellChecksum(snapshotCell);
            const quint64 appliedCellChecksum =
                terminalCellChecksum(appliedCell);
            if (snapshotCell.text.size() != 1
                || snapshotCell.text.at(0).unicode() != expected
                || appliedCell.text != snapshotCell.text
                || appliedCellChecksum != snapshotCellChecksum) {
                *error = QStringLiteral(
                    "materialized or retained cell state was unexpected");
                return false;
            }
            *snapshotChecksum =
                mixChecksum(*snapshotChecksum,
                            static_cast<quint64>(expectedRow) << 32
                                | static_cast<quint64>(column));
            *snapshotChecksum = mixChecksum(*snapshotChecksum,
                                            snapshotCell.text.at(0).unicode());
            *snapshotChecksum =
                mixChecksum(*snapshotChecksum, snapshotCellChecksum);
            *applyChecksum = mixChecksum(*applyChecksum,
                                         static_cast<quint64>(expectedRow) << 32
                                             | static_cast<quint64>(column));
            *applyChecksum =
                mixChecksum(*applyChecksum, appliedCell.text.at(0).unicode());
            *applyChecksum = mixChecksum(*applyChecksum, appliedCellChecksum);
        }
    }
    return true;
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

void printTiming(QTextStream &output, QStringView name,
                 const TimingSummary &summary)
{
    output << name << "_min_us=" << summary.minimumMicroseconds << ' ' << name
           << "_median_us=" << summary.medianMicroseconds << ' ' << name
           << "_p90_us=" << summary.percentile90Microseconds << ' ' << name
           << "_mean_us=" << summary.meanMicroseconds;
}

bool initializeAdapter(const BenchmarkOptions &options,
                       std::unique_ptr<GhosttyVtAdapter> *adapter,
                       TerminalFrame *frame, QString *error)
{
    GhosttyVtAdapter::Options adapterOptions;
    adapterOptions.geometry = {
        .columns = options.columns,
        .rows = options.rows,
        .cellWidthPixels = 8,
        .cellHeightPixels = 16,
        .surfaceWidthPixels = options.columns * 8,
        .surfaceHeightPixels = options.rows * 16,
    };
    *adapter = GhosttyVtAdapter::create(adapterOptions, {});
    if (*adapter == nullptr) {
        *error = QStringLiteral("failed to initialize libghostty adapter");
        return false;
    }

    (*adapter)->writeVt(fullFrameCommands(options, 0));
    GhosttyVtAdapter::RenderSnapshot initial;
    if ((*adapter)->renderFrame(&initial)
            != GhosttyVtAdapter::RenderResult::Ready
        || !initial.update.fullFrame
        || !applyTerminalUpdate(*frame, initial.update)) {
        *error = QStringLiteral("failed to initialize retained terminal frame");
        return false;
    }
    const QVector<int> noDirtyRows;
    quint64 snapshotChecksum = 1'469'598'103'934'665'603ULL;
    quint64 applyChecksum = 1'469'598'103'934'665'603ULL;
    return validateAndChecksum(initial.update, *frame, options,
                               Workload::FullFrame, noDirtyRows, 0,
                               &snapshotChecksum, &applyChecksum, error);
}

std::optional<Measurement> runBenchmark(const BenchmarkOptions &options,
                                        Workload workload, QString *error)
{
    std::unique_ptr<GhosttyVtAdapter> adapter;
    TerminalFrame frame;
    if (!initializeAdapter(options, &adapter, &frame, error)) {
        return std::nullopt;
    }

    const QVector<int> dirtyRows = dirtyRowIndices(options);
    const QByteArray fullVariants[]{
        fullFrameCommands(options, 1),
        fullFrameCommands(options, 2),
    };
    const QByteArray dirtyVariants[]{
        dirtyRowCommands(options, dirtyRows, 1),
        dirtyRowCommands(options, dirtyRows, 2),
    };

    Measurement measurement;
    measurement.snapshotNanoseconds.reserve(options.iterations);
    measurement.applyNanoseconds.reserve(options.iterations);
    measurement.endToEndNanoseconds.reserve(options.iterations);
    const int totalIterations = options.warmup + options.iterations;
    for (int iteration = 0; iteration < totalIterations; ++iteration) {
        const int variantIndex = iteration % 2;
        const int variant = variantIndex + 1;
        if (workload == Workload::FullFrame) {
            adapter->reset();
            adapter->writeVt(fullVariants[variantIndex]);
        } else {
            adapter->writeVt(dirtyVariants[variantIndex]);
        }

        GhosttyVtAdapter::RenderSnapshot snapshot;
        QElapsedTimer endToEndTimer;
        QElapsedTimer snapshotTimer;
        endToEndTimer.start();
        snapshotTimer.start();
        const GhosttyVtAdapter::RenderResult renderResult =
            adapter->renderFrame(&snapshot);
        const qint64 snapshotNanoseconds = snapshotTimer.nsecsElapsed();

        QElapsedTimer applyTimer;
        applyTimer.start();
        const bool applied =
            renderResult == GhosttyVtAdapter::RenderResult::Ready
            && applyTerminalUpdate(frame, snapshot.update);
        const qint64 applyNanoseconds = applyTimer.nsecsElapsed();
        const qint64 endToEndNanoseconds = endToEndTimer.nsecsElapsed();
        if (!applied) {
            *error = QStringLiteral(
                "adapter did not publish an applicable terminal update");
            return std::nullopt;
        }
        if (!validateAndChecksum(snapshot.update, frame, options, workload,
                                 dirtyRows, variant,
                                 &measurement.snapshotChecksum,
                                 &measurement.applyChecksum, error)) {
            return std::nullopt;
        }

        if (iteration >= options.warmup) {
            measurement.snapshotNanoseconds.append(snapshotNanoseconds);
            measurement.applyNanoseconds.append(applyNanoseconds);
            measurement.endToEndNanoseconds.append(endToEndNanoseconds);
        }
    }
    return measurement;
}

void printMeasurement(const BenchmarkOptions &options, Workload workload,
                      const Measurement &measurement)
{
    const TimingSummary snapshot = summarize(measurement.snapshotNanoseconds);
    const TimingSummary apply = summarize(measurement.applyNanoseconds);
    const TimingSummary endToEnd = summarize(measurement.endToEndNanoseconds);
    const quint64 rowsPerUpdate = workload == Workload::FullFrame
        ? static_cast<quint64>(options.rows)
        : static_cast<quint64>(options.dirtyRows);
    const quint64 cellsPerUpdate =
        static_cast<quint64>(options.columns) * rowsPerUpdate;
    const quint64 payloadBytes = cellsPerUpdate * sizeof(TerminalCell);
    const double measuredCells =
        static_cast<double>(cellsPerUpdate) * options.iterations;
    const double measuredPayloadGiB = static_cast<double>(payloadBytes)
        * options.iterations / (1'024.0 * 1'024.0 * 1'024.0);
    const auto seconds = [](qint64 nanoseconds) {
        return std::max(static_cast<double>(nanoseconds) / 1'000'000'000.0,
                        std::numeric_limits<double>::min());
    };

    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(2);
    output << "benchmark=terminal-frame-materialization benchmark_contract=1"
           << " workload="
           << (workload == Workload::FullFrame ? "full-frame" : "dirty-row")
           << " columns=" << options.columns << " rows=" << options.rows
           << " dirty_rows="
           << (workload == Workload::FullFrame ? options.rows
                                               : options.dirtyRows)
           << " cells_per_update=" << cellsPerUpdate
           << " terminal_cell_bytes=" << sizeof(TerminalCell)
           << " update_cell_payload_bytes=" << payloadBytes
           << " warmup=" << options.warmup
           << " iterations=" << options.iterations << '\n';
    output << "timing ";
    printTiming(output, QStringLiteral("snapshot"), snapshot);
    output << ' ';
    printTiming(output, QStringLiteral("apply"), apply);
    output << ' ';
    printTiming(output, QStringLiteral("end_to_end"), endToEnd);
    output << '\n';
    output << "throughput snapshot_cells_per_second="
           << measuredCells / seconds(snapshot.totalNanoseconds)
           << " snapshot_gib_per_second="
           << measuredPayloadGiB / seconds(snapshot.totalNanoseconds)
           << " apply_cells_per_second="
           << measuredCells / seconds(apply.totalNanoseconds)
           << " apply_gib_per_second="
           << measuredPayloadGiB / seconds(apply.totalNanoseconds)
           << " end_to_end_cells_per_second="
           << measuredCells / seconds(endToEnd.totalNanoseconds)
           << " end_to_end_updates_per_second="
           << static_cast<double>(options.iterations)
            / seconds(endToEnd.totalNanoseconds)
           << '\n';
    output << "validation snapshot_checksum=" << measurement.snapshotChecksum
           << " apply_checksum=" << measurement.applyChecksum << '\n';
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-frame-materialization"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Ghostty VT frame materialization and retained-update benchmark"));
    parser.addHelpOption();
    const QCommandLineOption workloadOption(
        QStringLiteral("workload"),
        QStringLiteral("Workload to run: full, dirty, or all."),
        QStringLiteral("name"), QStringLiteral("all"));
    const QCommandLineOption columnsOption(
        QStringLiteral("columns"), QStringLiteral("Terminal columns."),
        QStringLiteral("count"), QStringLiteral("160"));
    const QCommandLineOption rowsOption(
        QStringLiteral("rows"), QStringLiteral("Terminal rows."),
        QStringLiteral("count"), QStringLiteral("48"));
    const QCommandLineOption dirtyRowsOption(
        QStringLiteral("dirty-rows"),
        QStringLiteral("Rows rewritten by each dirty-row update."),
        QStringLiteral("count"), QStringLiteral("4"));
    const QCommandLineOption warmupOption(
        QStringLiteral("warmup"), QStringLiteral("Unmeasured updates."),
        QStringLiteral("count"), QStringLiteral("10"));
    const QCommandLineOption iterationsOption(
        QStringLiteral("iterations"), QStringLiteral("Measured updates."),
        QStringLiteral("count"), QStringLiteral("100"));
    parser.addOptions({workloadOption, columnsOption, rowsOption,
                       dirtyRowsOption, warmupOption, iterationsOption});
    parser.process(application);

    BenchmarkOptions options;
    const std::optional<int> columns =
        integerOption(parser.value(columnsOption));
    const std::optional<int> rows = integerOption(parser.value(rowsOption));
    const std::optional<int> dirtyRows =
        integerOption(parser.value(dirtyRowsOption));
    const std::optional<int> warmup =
        integerOption(parser.value(warmupOption), true);
    const std::optional<int> iterations =
        integerOption(parser.value(iterationsOption));
    if (!columns || !rows || !dirtyRows || !warmup || !iterations) {
        QTextStream(stderr)
            << "dimensions, dirty rows, and iterations must be positive; "
               "warmup must be non-negative\n";
        return 2;
    }
    options.columns = *columns;
    options.rows = *rows;
    options.dirtyRows = *dirtyRows;
    options.warmup = *warmup;
    options.iterations = *iterations;
    const quint64 frameCells = static_cast<quint64>(options.columns)
        * static_cast<quint64>(options.rows);
    if (options.columns > std::numeric_limits<quint16>::max()
        || options.rows > std::numeric_limits<quint16>::max()
        || options.dirtyRows > options.rows
        || frameCells > maximumBenchmarkCells) {
        QTextStream(stderr)
            << "dimensions exceed libghostty limits, dirty-rows exceeds rows, "
               "or the frame exceeds 1000000 cells\n";
        return 2;
    }

    const QString workload = parser.value(workloadOption).toLower();
    if (workload != QStringLiteral("full")
        && workload != QStringLiteral("dirty")
        && workload != QStringLiteral("all")) {
        QTextStream(stderr) << "workload must be full, dirty, or all\n";
        return 2;
    }

    const auto run = [&](Workload selected) {
        QString error;
        const std::optional<Measurement> measurement =
            runBenchmark(options, selected, &error);
        if (!measurement) {
            QTextStream(stderr) << error << '\n';
            return false;
        }
        printMeasurement(options, selected, *measurement);
        return true;
    };
    if ((workload == QStringLiteral("full")
         || workload == QStringLiteral("all"))
        && !run(Workload::FullFrame)) {
        return 1;
    }
    if ((workload == QStringLiteral("dirty")
         || workload == QStringLiteral("all"))
        && !run(Workload::DirtyRows)) {
        return 1;
    }
    return 0;
}
