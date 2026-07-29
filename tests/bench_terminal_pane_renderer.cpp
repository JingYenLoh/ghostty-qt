#include "launch_options.h"
#include "terminal_cell_metrics.h"
#include "terminal_controller.h"
#include "terminal_pane.h"
#include "terminal_pane_render_probe_p.h"
#include "terminal_types.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTest>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>

namespace {

struct GridSize {
    int columns = 0;
    int rows = 0;
};

struct TimingSummary {
    double minimumMicroseconds = 0.0;
    double medianMicroseconds = 0.0;
    double percentile90Microseconds = 0.0;
    double meanMicroseconds = 0.0;
};

struct ScenarioResult {
    QString name;
    TimingSummary timing;
    std::optional<quint64> solidCellVisits;
    quint64 expectedSolidCellVisits = 0;
};

template <typename Snapshot>
std::optional<quint64> solidCellVisitCount(const Snapshot &snapshot)
{
    if constexpr (requires { snapshot.solidCellVisitCount; }) {
        return snapshot.solidCellVisitCount;
    }
    return std::nullopt;
}

void useSystemFixedFont(LaunchOptions &options)
{
    options.typography.face(TerminalFontRole::Regular).families = {
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family(),
    };
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() >= timeoutMilliseconds) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qWait(1);
    }
    return true;
}

TimingSummary summarize(QVector<qint64> samples)
{
    std::ranges::sort(samples);
    const qsizetype count = samples.size();
    const auto microseconds = [](qint64 nanoseconds) {
        return static_cast<double>(nanoseconds) / 1'000.0;
    };
    const qsizetype medianIndex = count / 2;
    const qsizetype percentile90Index =
        std::min(count - 1,
                 static_cast<qsizetype>(
                     std::ceil(static_cast<double>(count) * 0.9) - 1.0));
    const qint64 total =
        std::accumulate(samples.cbegin(), samples.cend(), qint64{0});
    return {
        .minimumMicroseconds = microseconds(samples.constFirst()),
        .medianMicroseconds = microseconds(samples.at(medianIndex)),
        .percentile90Microseconds = microseconds(samples.at(percentile90Index)),
        .meanMicroseconds = microseconds(total) / static_cast<double>(count),
    };
}

class RendererBenchmark {
public:
    explicit RendererBenchmark(GridSize grid)
        : grid_(grid)
    {
        LaunchOptions options;
        options.workingDirectory = QDir::currentPath();
        options.program = {QStringLiteral("/bin/true")};
        options.hold = true;
        useSystemFixedFont(options);
        options.appearance.foregroundColor = Qt::white;
        options.appearance.backgroundColor = Qt::black;
        options.appearance.cursorColor =
            TerminalColorValue::fromColor(QColor(QStringLiteral("#f0f0f0")));
        options.appearance.cursorTextColor =
            TerminalColorValue::fromColor(QColor(QStringLiteral("#101010")));

        const TerminalCellMetrics metrics =
            terminalCellMetrics(options.typography);
        window_.setColor(Qt::black);
        window_.resize(
            qCeil(metrics.cellWidth * static_cast<qreal>(grid_.columns)),
            qCeil(metrics.cellHeight * static_cast<qreal>(grid_.rows)));
        pane_ = new TerminalPane(options, window_.contentItem(), std::nullopt,
                                 TerminalSessionStartMode::Deferred);
        pane_->setSize(window_.size());
        controller_ = pane_->findChild<TerminalController *>();
    }

    ~RendererBenchmark()
    {
        window_.close();
        delete pane_;
    }

    bool initialize()
    {
        if (controller_ == nullptr) return false;
        window_.show();
        if (!waitUntil([this] { return window_.isExposed(); }, 3'000)) {
            return false;
        }
        pane_->forceActiveFocus();
        publishFullFrame(false);
        return render();
    }

    ScenarioResult metadata(int warmupIterations, int measuredIterations)
    {
        bool odd = false;
        return measure(QStringLiteral("metadata-only"), warmupIterations,
                       measuredIterations, 0, [this, &odd] {
                           odd = !odd;
                           TerminalUpdate update;
                           update.columns = grid_.columns;
                           update.rows = grid_.rows;
                           update.scrollbarChanged = true;
                           update.scrollTotal = 10'000;
                           update.scrollOffset = odd ? 3'000 : 3'001;
                           update.scrollLength =
                               static_cast<quint64>(grid_.rows);
                           update.contentRevision = ++revision_;
                           controller_->terminalUpdated(update);
                       });
    }

    ScenarioResult oneDirtyRow(int warmupIterations, int measuredIterations)
    {
        bool odd = false;
        return measure(QStringLiteral("one-dirty-row"), warmupIterations,
                       measuredIterations, static_cast<quint64>(grid_.columns),
                       [this, &odd] {
                           odd = !odd;
                           TerminalUpdate update;
                           update.columns = grid_.columns;
                           update.rows = grid_.rows;
                           update.dirtyRows.append(makeRow(
                               grid_.rows / 2,
                               odd ? QColor(QStringLiteral("#18222c"))
                                   : QColor(QStringLiteral("#202a34"))));
                           update.contentRevision = ++revision_;
                           controller_->terminalUpdated(update);
                       });
    }

    ScenarioResult blockCursorMove(int warmupIterations, int measuredIterations)
    {
        cursorRow_ = 0;
        publishCursor();
        if (!render()) return {};

        return measure(QStringLiteral("block-cursor-row-move"),
                       warmupIterations, measuredIterations,
                       static_cast<quint64>(2 * grid_.columns), [this] {
                           cursorRow_ = cursorRow_ == 0 ? grid_.rows - 1 : 0;
                           publishCursor();
                       });
    }

    ScenarioResult fullInvalidation(int warmupIterations,
                                    int measuredIterations)
    {
        bool odd = false;
        return measure(QStringLiteral("full-invalidation"), warmupIterations,
                       measuredIterations,
                       static_cast<quint64>(grid_.columns * grid_.rows),
                       [this, &odd] {
                           odd = !odd;
                           publishFullFrame(odd);
                       });
    }

private:
    TerminalRowUpdate makeRow(int row, const QColor &background) const
    {
        TerminalRowUpdate update;
        update.row = row;
        update.cells.resize(grid_.columns);
        for (int column = 0; column < grid_.columns; ++column) {
            TerminalCell &cell = update.cells[column];
            cell.foreground = QColor(QStringLiteral("#d8dee9"));
            cell.background = background;
            if (column % 16 == 0) {
                cell.text = QString(QChar(0x2588));
            }
        }
        return update;
    }

    void publishFullFrame(bool alternate)
    {
        TerminalUpdate update;
        update.columns = grid_.columns;
        update.rows = grid_.rows;
        update.fullFrame = true;
        update.foreground = QColor(QStringLiteral("#d8dee9"));
        update.background = Qt::black;
        update.cursorColor = QColor(QStringLiteral("#f0f0f0"));
        update.cursorColorExplicit = true;
        update.contentRevision = ++revision_;
        const QColor first = alternate ? QColor(QStringLiteral("#18222c"))
                                       : QColor(QStringLiteral("#202a34"));
        const QColor second = alternate ? QColor(QStringLiteral("#202a34"))
                                        : QColor(QStringLiteral("#18222c"));
        update.dirtyRows.reserve(grid_.rows);
        for (int row = 0; row < grid_.rows; ++row) {
            update.dirtyRows.append(
                makeRow(row, row % 2 == 0 ? first : second));
        }
        controller_->terminalUpdated(update);
    }

    void publishCursor()
    {
        TerminalUpdate update;
        update.columns = grid_.columns;
        update.rows = grid_.rows;
        update.cursorChanged = true;
        update.cursorVisible = true;
        update.cursorBlinking = false;
        update.cursorColumn = grid_.columns / 2;
        update.cursorRow = cursorRow_;
        update.cursorStyle = 1;
        update.cursorColumnSpan = 1;
        update.contentRevision = ++revision_;
        controller_->terminalUpdated(update);
    }

    bool render()
    {
        const QImage image = window_.grabWindow();
        return !image.isNull();
    }

    ScenarioResult measure(const QString &name, int warmupIterations,
                           int measuredIterations,
                           quint64 expectedSolidCellVisitsPerFrame,
                           const std::function<void()> &prepare)
    {
        for (int iteration = 0; iteration < warmupIterations; ++iteration) {
            prepare();
            if (!render()) return {};
        }

        const TerminalPaneRenderProbeSnapshot before =
            terminalPaneRenderProbe(pane_);
        QVector<qint64> samples;
        samples.reserve(measuredIterations);
        for (int iteration = 0; iteration < measuredIterations; ++iteration) {
            QElapsedTimer timer;
            timer.start();
            prepare();
            if (!render()) return {};
            samples.append(timer.nsecsElapsed());
        }
        const TerminalPaneRenderProbeSnapshot after =
            terminalPaneRenderProbe(pane_);
        const std::optional<quint64> beforeVisits = solidCellVisitCount(before);
        const std::optional<quint64> afterVisits = solidCellVisitCount(after);
        return {
            .name = name,
            .timing = summarize(std::move(samples)),
            .solidCellVisits =
                beforeVisits.has_value() && afterVisits.has_value()
                ? std::optional<quint64>(*afterVisits - *beforeVisits)
                : std::nullopt,
            .expectedSolidCellVisits = expectedSolidCellVisitsPerFrame
                * static_cast<quint64>(measuredIterations),
        };
    }

    GridSize grid_;
    QQuickWindow window_;
    TerminalPane *pane_ = nullptr;
    TerminalController *controller_ = nullptr;
    quint64 revision_ = 0;
    int cursorRow_ = 0;
};

std::optional<int> positiveInteger(const QString &value)
{
    bool ok = false;
    const int result = value.toInt(&ok);
    if (!ok || result <= 0) return std::nullopt;
    return result;
}

int runGrid(GridSize grid, int warmupIterations, int measuredIterations,
            QTextStream &output)
{
    RendererBenchmark benchmark(grid);
    if (!benchmark.initialize()) {
        output << "failed to initialize " << grid.columns << 'x' << grid.rows
               << '\n';
        return 1;
    }

    const QVector<ScenarioResult> results{
        benchmark.metadata(warmupIterations, measuredIterations),
        benchmark.oneDirtyRow(warmupIterations, measuredIterations),
        benchmark.blockCursorMove(warmupIterations, measuredIterations),
        benchmark.fullInvalidation(warmupIterations, measuredIterations),
    };
    for (const ScenarioResult &result : results) {
        if (result.name.isEmpty()) {
            output << "failed to render " << grid.columns << 'x' << grid.rows
                   << '\n';
            return 1;
        }
        output << grid.columns << 'x' << grid.rows << ' ' << result.name
               << " median_us="
               << QString::number(result.timing.medianMicroseconds, 'f', 1)
               << " p90_us="
               << QString::number(result.timing.percentile90Microseconds, 'f',
                                  1)
               << " mean_us="
               << QString::number(result.timing.meanMicroseconds, 'f', 1)
               << " min_us="
               << QString::number(result.timing.minimumMicroseconds, 'f', 1)
               << " solid_cell_visits_per_frame=";
        if (result.solidCellVisits.has_value()) {
            const double visitsPerFrame =
                static_cast<double>(*result.solidCellVisits)
                / static_cast<double>(measuredIterations);
            output << QString::number(visitsPerFrame, 'f', 1) << " expected="
                   << QString::number(
                          static_cast<double>(result.expectedSolidCellVisits)
                              / static_cast<double>(measuredIterations),
                          'f', 1);
        } else {
            output << "unavailable";
        }
        output << '\n';
        if (result.solidCellVisits.has_value()
            && *result.solidCellVisits != result.expectedSolidCellVisits) {
            output << "unexpected solid-cell visit count for " << result.name
                   << '\n';
            return 2;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_SCALE_FACTOR", "1");
    QStandardPaths::setTestModeEnabled(true);

    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-pane-renderer"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Warmed ghostty-qt incremental-render benchmark"));
    parser.addHelpOption();
    const QCommandLineOption warmupOption(
        QStringList{QStringLiteral("w"), QStringLiteral("warmup")},
        QStringLiteral("Warmup frames per scenario."), QStringLiteral("count"),
        QStringLiteral("8"));
    const QCommandLineOption iterationsOption(
        QStringList{QStringLiteral("n"), QStringLiteral("iterations")},
        QStringLiteral("Measured frames per scenario."),
        QStringLiteral("count"), QStringLiteral("30"));
    parser.addOption(warmupOption);
    parser.addOption(iterationsOption);
    parser.process(application);

    const std::optional<int> warmup =
        positiveInteger(parser.value(warmupOption));
    const std::optional<int> iterations =
        positiveInteger(parser.value(iterationsOption));
    if (!warmup.has_value() || !iterations.has_value()) {
        QTextStream(stderr)
            << "--warmup and --iterations must be positive integers\n";
        return 2;
    }

    QTextStream output(stdout);
    output << "backend=software warmup=" << *warmup
           << " iterations=" << *iterations << '\n';
    const QVector<GridSize> grids{
        {.columns = 120, .rows = 40},
        {.columns = 240, .rows = 80},
    };
    for (const GridSize grid : grids) {
        const int result = runGrid(grid, *warmup, *iterations, output);
        if (result != 0) return result;
    }
    return 0;
}
