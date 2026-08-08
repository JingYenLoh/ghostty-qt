#include "launch_options.h"
#include "renderdoc_capture.h"
#include "terminal_cell_metrics.h"
#include "terminal_controller.h"
#include "terminal_kitty_graphics.h"
#include "terminal_pane.h"
#include "terminal_pane_render_probe_p.h"
#include "terminal_types.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QQuickGraphicsConfiguration>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTest>
#include <QTextStream>
#include <QUrl>
#include <rhi/qrhi.h>

#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanInstance>
#define GHOSTTY_QT_PANE_BENCH_HAS_VULKAN 1
#else
#define GHOSTTY_QT_PANE_BENCH_HAS_VULKAN 0
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>

namespace {

constexpr int kittyPlacementCount = 512;
constexpr quint64 kittyTextureLogicalBytes = 64ULL * 64ULL * 4ULL;
constexpr quint32 discretionaryLigaturesTag = 0x646c6967U;

struct GridSize {
    int columns = 0;
    int rows = 0;
};

int logicalExtentForCells(qreal cellExtent, int cellCount,
                          qreal devicePixelRatio)
{
    const int cellPixels = qMax(1, qRound(cellExtent * devicePixelRatio));
    return qMax(
        1,
        qCeil(static_cast<qreal>(cellPixels) * cellCount / devicePixelRatio));
}

enum class GraphicsApi {
    Software,
    OpenGl,
    Vulkan,
};

enum class InitializationResult {
    Success,
    BackendUnavailable,
    Failure,
};

QStringView graphicsApiName(GraphicsApi graphicsApi)
{
    switch (graphicsApi) {
    case GraphicsApi::Software: return u"software";
    case GraphicsApi::OpenGl: return u"opengl";
    case GraphicsApi::Vulkan: return u"vulkan";
    }
    return {};
}

QStringView deviceTypeName(QRhiDriverInfo::DeviceType deviceType)
{
    switch (deviceType) {
    case QRhiDriverInfo::UnknownDevice: return u"unknown";
    case QRhiDriverInfo::IntegratedDevice: return u"integrated";
    case QRhiDriverInfo::DiscreteDevice: return u"discrete";
    case QRhiDriverInfo::ExternalDevice: return u"external";
    case QRhiDriverInfo::VirtualDevice: return u"virtual";
    case QRhiDriverInfo::CpuDevice: return u"cpu";
    }
    return u"unknown";
}

QString outputToken(QStringView value)
{
    return QString::fromLatin1(
        QUrl::toPercentEncoding(value.toString(), QByteArrayLiteral("._-")));
}

struct TimingSummary {
    double minimumMicroseconds = 0.0;
    double medianMicroseconds = 0.0;
    double percentile90Microseconds = 0.0;
    double meanMicroseconds = 0.0;
};

struct FrameTiming {
    std::optional<qint64> cpuUpdateNanoseconds;
    std::optional<qint64> cpuRecordNanoseconds;
    std::optional<qint64> cpuCompletionNanoseconds;
    qint64 cpuTotalNanoseconds = 0;
    std::optional<qint64> gpuNanoseconds;
};

struct ProbeDelta {
    quint64 paintSerial = 0;
    quint64 solidCellVisits = 0;
    quint64 textRowBuilds = 0;
    quint64 textLayouts = 0;
    quint64 textFallbackCells = 0;
    quint64 nativeTextSubmissions = 0;
    quint64 nativeTextCells = 0;
    quint64 batchedGlyphs = 0;
    quint64 glyphBatchGeometryWrites = 0;
    quint64 glyphBatchNodeCreations = 0;
    quint64 glyphBatchAllocations = 0;
    quint64 glyphAtlasUploads = 0;
    qint64 glyphAtlasEntryCount = 0;
    qint64 glyphAtlasBytes = 0;
    quint64 kittyTextureUploads = 0;
    quint64 kittyNodeCreations = 0;
    quint64 kittyNodeDeletions = 0;
    quint64 kittyGeometryWrites = 0;
    quint64 kittyMaterialAssignments = 0;
    quint64 kittyTextureSetEvictions = 0;
    qint64 kittyTextureSetCount = 0;

    ProbeDelta &operator+=(const ProbeDelta &other)
    {
        paintSerial += other.paintSerial;
        solidCellVisits += other.solidCellVisits;
        textRowBuilds += other.textRowBuilds;
        textLayouts += other.textLayouts;
        textFallbackCells += other.textFallbackCells;
        nativeTextSubmissions += other.nativeTextSubmissions;
        nativeTextCells += other.nativeTextCells;
        batchedGlyphs += other.batchedGlyphs;
        glyphBatchGeometryWrites += other.glyphBatchGeometryWrites;
        glyphBatchNodeCreations += other.glyphBatchNodeCreations;
        glyphBatchAllocations += other.glyphBatchAllocations;
        glyphAtlasUploads += other.glyphAtlasUploads;
        glyphAtlasEntryCount += other.glyphAtlasEntryCount;
        glyphAtlasBytes += other.glyphAtlasBytes;
        kittyTextureUploads += other.kittyTextureUploads;
        kittyNodeCreations += other.kittyNodeCreations;
        kittyNodeDeletions += other.kittyNodeDeletions;
        kittyGeometryWrites += other.kittyGeometryWrites;
        kittyMaterialAssignments += other.kittyMaterialAssignments;
        kittyTextureSetEvictions += other.kittyTextureSetEvictions;
        kittyTextureSetCount += other.kittyTextureSetCount;
        return *this;
    }
};

struct ScenarioResult {
    QString name;
    bool captureFrame = false;
    std::optional<TimingSummary> cpuUpdateTiming;
    std::optional<TimingSummary> cpuRecordTiming;
    std::optional<TimingSummary> cpuCompletionTiming;
    TimingSummary cpuTotalTiming;
    std::optional<TimingSummary> gpuTiming;
    int validGpuSamples = 0;
    ProbeDelta probeDelta;
    ProbeDelta expectedProbeDelta;
    qsizetype finalKittyTextureSetCount = 0;
    qsizetype expectedFinalKittyTextureSetCount = 0;
    quint64 finalKittyTextureBytes = 0;
    quint64 expectedFinalKittyTextureBytes = 0;
    qsizetype finalNativeTextNodeCount = 0;
    std::optional<qsizetype> expectedFinalNativeTextNodeCount;
    quint64 finalGlyphAtlasSerial = 0;
    std::optional<quint64> expectedFinalGlyphAtlasSerial;
    QVector<quint64> finalRowContainerSerials;
    std::optional<QVector<quint64>> expectedFinalRowContainerSerials;
    QVector<quint64> finalRowGlyphBatchSerials;
    std::optional<QVector<quint64>> expectedFinalRowGlyphBatchSerials;
    QVector<quint64> finalRowNativeTextNodeSerials;
    std::optional<QVector<quint64>> expectedFinalRowNativeTextNodeSerials;
    qsizetype finalGlyphAtlasEntryCount = 0;
    quint64 finalGlyphAtlasBytes = 0;
    int measuredFrames = 0;
};

ProbeDelta operator-(const TerminalPaneRenderProbeSnapshot &after,
                     const TerminalPaneRenderProbeSnapshot &before)
{
    ProbeDelta result{
        .paintSerial = after.paintSerial - before.paintSerial,
        .solidCellVisits =
            after.solidCellVisitCount - before.solidCellVisitCount,
        .nativeTextSubmissions =
            after.nativeTextSubmissionCount - before.nativeTextSubmissionCount,
        .nativeTextCells =
            after.nativeTextCellCount - before.nativeTextCellCount,
        .batchedGlyphs = after.batchedGlyphCount - before.batchedGlyphCount,
        .glyphBatchGeometryWrites = after.glyphBatchGeometryWriteCount
            - before.glyphBatchGeometryWriteCount,
        .glyphBatchNodeCreations = after.glyphBatchNodeCreationCount
            - before.glyphBatchNodeCreationCount,
        .glyphBatchAllocations =
            after.glyphBatchAllocationCount - before.glyphBatchAllocationCount,
        .glyphAtlasUploads =
            after.glyphAtlasUploadCount - before.glyphAtlasUploadCount,
        .glyphAtlasEntryCount = static_cast<qint64>(after.glyphAtlasEntryCount)
            - static_cast<qint64>(before.glyphAtlasEntryCount),
        .glyphAtlasBytes = static_cast<qint64>(after.glyphAtlasBytes)
            - static_cast<qint64>(before.glyphAtlasBytes),
        .kittyTextureUploads = after.kittyGraphicsTextureUploadCount
            - before.kittyGraphicsTextureUploadCount,
        .kittyNodeCreations = after.kittyGraphicsNodeCreationCount
            - before.kittyGraphicsNodeCreationCount,
        .kittyNodeDeletions = after.kittyGraphicsNodeDeletionCount
            - before.kittyGraphicsNodeDeletionCount,
        .kittyGeometryWrites = after.kittyGraphicsGeometryWriteCount
            - before.kittyGraphicsGeometryWriteCount,
        .kittyMaterialAssignments = after.kittyGraphicsMaterialAssignmentCount
            - before.kittyGraphicsMaterialAssignmentCount,
        .kittyTextureSetEvictions = after.kittyGraphicsTextureSetEvictionCount
            - before.kittyGraphicsTextureSetEvictionCount,
        .kittyTextureSetCount =
            static_cast<qint64>(after.kittyGraphicsTextureCount)
            - static_cast<qint64>(before.kittyGraphicsTextureCount),
    };
    for (qsizetype row = 0; row < after.rowBuildCounts.size(); ++row) {
        const quint64 beforeBuilds = row < before.rowBuildCounts.size()
            ? before.rowBuildCounts.at(row)
            : 0;
        const quint64 afterBuilds = after.rowBuildCounts.at(row);
        if (afterBuilds <= beforeBuilds) continue;

        const quint64 builds = afterBuilds - beforeBuilds;
        result.textRowBuilds += builds;
        if (row < after.rowLayoutCounts.size()) {
            result.textLayouts += builds * after.rowLayoutCounts.at(row);
        }
        if (row < after.rowFallbackCellCounts.size()) {
            result.textFallbackCells +=
                builds * after.rowFallbackCellCounts.at(row);
        }
    }
    return result;
}

ProbeDelta operator*(ProbeDelta delta, quint64 count)
{
    delta.paintSerial *= count;
    delta.solidCellVisits *= count;
    delta.textRowBuilds *= count;
    delta.textLayouts *= count;
    delta.textFallbackCells *= count;
    delta.nativeTextSubmissions *= count;
    delta.nativeTextCells *= count;
    delta.batchedGlyphs *= count;
    delta.glyphBatchGeometryWrites *= count;
    delta.glyphBatchNodeCreations *= count;
    delta.glyphBatchAllocations *= count;
    delta.glyphAtlasUploads *= count;
    delta.glyphAtlasEntryCount *= static_cast<qint64>(count);
    delta.glyphAtlasBytes *= static_cast<qint64>(count);
    delta.kittyTextureUploads *= count;
    delta.kittyNodeCreations *= count;
    delta.kittyNodeDeletions *= count;
    delta.kittyGeometryWrites *= count;
    delta.kittyMaterialAssignments *= count;
    delta.kittyTextureSetEvictions *= count;
    delta.kittyTextureSetCount *= static_cast<qint64>(count);
    return delta;
}

void useSystemFixedFont(LaunchOptions &options)
{
    options.typography.face(TerminalFontRole::Regular).families = {
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family(),
    };
}

bool useLigatureBenchmarkFont(LaunchOptions &options, QString *error)
{
    const QString path =
        QFINDTESTDATA("../ghostty/src/font/res/Inconsolata-Regular.ttf");
    if (path.isEmpty()) {
        *error =
            QStringLiteral("pinned Inconsolata benchmark font was not found");
        return false;
    }
    const int fontId = QFontDatabase::addApplicationFont(path);
    if (fontId < 0) {
        *error =
            QStringLiteral("unable to load pinned Inconsolata benchmark font");
        return false;
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    if (families.isEmpty()) {
        *error =
            QStringLiteral("pinned Inconsolata benchmark font has no family");
        return false;
    }
    options.typography.face(TerminalFontRole::Regular).families = {
        families.constFirst(),
    };
    options.typography.features = {
        {.tag = discretionaryLigaturesTag, .value = 1},
    };
    return true;
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
    const qsizetype percentile90Index =
        std::min(count - 1,
                 static_cast<qsizetype>(
                     std::ceil(static_cast<double>(count) * 0.9) - 1.0));
    const qint64 total =
        std::accumulate(samples.cbegin(), samples.cend(), qint64{0});
    const double medianNanoseconds = count % 2 == 0
        ? (static_cast<double>(samples.at(count / 2 - 1))
           + static_cast<double>(samples.at(count / 2)))
            / 2.0
        : static_cast<double>(samples.at(count / 2));
    return {
        .minimumMicroseconds = microseconds(samples.constFirst()),
        .medianMicroseconds = medianNanoseconds / 1'000.0,
        .percentile90Microseconds = microseconds(samples.at(percentile90Index)),
        .meanMicroseconds = microseconds(total) / static_cast<double>(count),
    };
}

bool containsNonBlackPixel(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qRed(pixel) != 0 || qGreen(pixel) != 0 || qBlue(pixel) != 0) {
                return true;
            }
        }
    }
    return false;
}

bool containsColorPixel(const QImage &image, const QColor &expected,
                        int tolerance = 3)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor actual = image.pixelColor(x, y);
            if (std::abs(actual.red() - expected.red()) <= tolerance
                && std::abs(actual.green() - expected.green()) <= tolerance
                && std::abs(actual.blue() - expected.blue()) <= tolerance) {
                return true;
            }
        }
    }
    return false;
}

class RendererBenchmark {
public:
    RendererBenchmark(GridSize grid, GraphicsApi graphicsApi,
                      bool renderDocCapture)
        : grid_(grid)
        , graphicsApi_(graphicsApi)
        , renderControl_(graphicsApi == GraphicsApi::Software
                             ? nullptr
                             : std::make_unique<QQuickRenderControl>())
        , window_(renderControl_ != nullptr
                      ? std::make_unique<QQuickWindow>(renderControl_.get())
                      : std::make_unique<QQuickWindow>())
    {
        LaunchOptions options;
        options.workingDirectory = QDir::currentPath();
        options.program = {QStringLiteral("/bin/true")};
        options.hold = true;
        if (!useLigatureBenchmarkFont(options, &fontError_)) {
            useSystemFixedFont(options);
        }
        options.appearance.foregroundColor = Qt::white;
        options.appearance.backgroundColor = Qt::black;
        options.appearance.cursorColor =
            TerminalColorValue::fromColor(QColor(QStringLiteral("#f0f0f0")));
        options.appearance.cursorTextColor =
            TerminalColorValue::fromColor(QColor(QStringLiteral("#101010")));
        options.appearance.boldColor = {
            .kind = TerminalBoldColorKind::Color,
            .color = QColor(QStringLiteral("#ff3300")),
        };

        const qreal devicePixelRatio = window_->devicePixelRatio();
        const TerminalCellMetrics metrics =
            terminalCellMetrics(options.typography, devicePixelRatio);
        logicalSize_ =
            QSize(logicalExtentForCells(metrics.cellWidth, grid_.columns,
                                        devicePixelRatio),
                  logicalExtentForCells(metrics.cellHeight, grid_.rows,
                                        devicePixelRatio));
        window_->setColor(Qt::black);
        window_->setGeometry(QRect(QPoint{}, logicalSize_));
        window_->contentItem()->setSize(logicalSize_);
        if (renderControl_ != nullptr) {
            QQuickGraphicsConfiguration configuration;
            configuration.setTimestamps(!renderDocCapture);
            configuration.setDebugMarkers(renderDocCapture);
            window_->setGraphicsConfiguration(configuration);
        }
        pane_ = new TerminalPane(options, window_->contentItem(), std::nullopt,
                                 TerminalSessionStartMode::Deferred);
        pane_->setSize(logicalSize_);
        controller_ = pane_->findChild<TerminalController *>();
    }

    ~RendererBenchmark()
    {
        delete pane_;
        pane_ = nullptr;
        if (renderControl_ == nullptr) {
            window_->close();
            return;
        }
        window_->setRenderTarget({});
        renderControl_->invalidate();
        renderTarget_.reset();
        renderPassDescriptor_.reset();
        depthStencil_.reset();
        colorBuffer_.reset();
    }

    InitializationResult initialize(QString *error)
    {
        if (!fontError_.isEmpty()) {
            *error = fontError_;
            return InitializationResult::Failure;
        }
        if (controller_ == nullptr) {
            *error = QStringLiteral("terminal controller was not created");
            return InitializationResult::Failure;
        }
        if (renderControl_ == nullptr) {
            window_->show();
            if (!waitUntil([this] { return window_->isExposed(); }, 3'000)) {
                *error =
                    QStringLiteral("software benchmark window was not exposed");
                return InitializationResult::BackendUnavailable;
            }
            framebufferSize_ =
                QSize(qMax(1,
                           qRound(static_cast<qreal>(logicalSize_.width())
                                  * window_->devicePixelRatio())),
                      qMax(1,
                           qRound(static_cast<qreal>(logicalSize_.height())
                                  * window_->devicePixelRatio())));
        } else {
            const InitializationResult initialized = initializeRhi(error);
            if (initialized != InitializationResult::Success) {
                return initialized;
            }
        }
        pane_->forceActiveFocus();
        publishFullFrame(false);
        if (!renderUntimed(error, true)) return InitializationResult::Failure;

        // Exercise the terminal-owned glyph batch against a black background
        // before timing anything. This catches atlas/shader failures that the
        // colored-cell validation above would otherwise conceal.
        publishTextFrame(u"ab+cd=ef+gh-");
        if (!renderUntimed(error, true)) return InitializationResult::Failure;

        const QColor kittyValidationColor(23, 211, 149);
        publishKitty(makeKittySnapshot(
            makeKittyImage(++kittyGeneration_, kittyValidationColor), 0));
        if (!renderUntimed(error, true, kittyValidationColor)) {
            return InitializationResult::Failure;
        }

        // Validate straight-alpha upload and source-over composition outside
        // the measured scenarios on every available RHI backend. The expected
        // color is QColor(203, 61, 149, 128) over the pane's black global
        // background using byte-linear RGBA8 blending.
        const QColor translucentValidationColor(203, 61, 149);
        const QColor translucentExpectedColor(102, 31, 75);
        publishKitty(makeKittySnapshot(
            makeKittyImage(++kittyGeneration_, translucentValidationColor, 128),
            1, 1));
        if (!renderUntimed(error, true, translucentExpectedColor)) {
            return InitializationResult::Failure;
        }

        publishKitty(nullptr);
        return renderUntimed(error) ? InitializationResult::Success
                                    : InitializationResult::Failure;
    }

    QString rhiBackendName() const
    {
        return renderControl_ != nullptr && renderControl_->rhi() != nullptr
            ? QString::fromLatin1(renderControl_->rhi()->backendName())
            : QStringLiteral("software-scenegraph");
    }

    qreal devicePixelRatio() const noexcept
    {
        return window_->devicePixelRatio();
    }

    QSize framebufferSize() const noexcept { return framebufferSize_; }

    QSize logicalSize() const noexcept { return logicalSize_; }

    const std::optional<QRhiDriverInfo> &driverInfo() const noexcept
    {
        return driverInfo_;
    }

    ScenarioResult metadata(int warmupIterations, int measuredIterations,
                            RenderDocCapture *capture, QString *error)
    {
        bool odd = false;
        return measure(
            QStringLiteral("metadata"), warmupIterations, measuredIterations,
            {.paintSerial = 1}, 0, 0, {},
            [this, &odd] {
                odd = !odd;
                TerminalUpdate update;
                update.columns = grid_.columns;
                update.rows = grid_.rows;
                update.scrollbarChanged = true;
                update.scrollTotal = 10'000;
                update.scrollOffset = odd ? 3'000 : 3'001;
                update.scrollLength = static_cast<quint64>(grid_.rows);
                update.contentRevision = ++revision_;
                controller_->terminalUpdated(update);
            },
            capture, error);
    }

    ScenarioResult oneDirtyRow(int warmupIterations, int measuredIterations,
                               RenderDocCapture *capture, QString *error)
    {
        bool odd = false;
        return measure(
            QStringLiteral("one-dirty-row"), warmupIterations,
            measuredIterations,
            {.paintSerial = 1,
             .solidCellVisits = static_cast<quint64>(grid_.columns),
             .textRowBuilds = 1,
             .textLayouts = 1,
             .nativeTextSubmissions = 1,
             .nativeTextCells = nativeControlCellsPerRow()},
            0, 0, {},
            [this, &odd] {
                odd = !odd;
                TerminalUpdate update;
                update.columns = grid_.columns;
                update.rows = grid_.rows;
                update.dirtyRows.append(
                    makeRow(grid_.rows / 2,
                            odd ? QColor(QStringLiteral("#18222c"))
                                : QColor(QStringLiteral("#202a34"))));
                update.contentRevision = ++revision_;
                controller_->terminalUpdated(update);
            },
            capture, error);
    }

    ScenarioResult textAsciiDirtyRow(int warmupIterations,
                                     int measuredIterations,
                                     RenderDocCapture *capture, QString *error)
    {
        return textShapingScenario(QStringLiteral("text-ascii-dirty-row"),
                                   u"ab+cd=ef+gh-", u"ij-kl+mn=op+", false,
                                   true, warmupIterations, measuredIterations,
                                   capture, error);
    }

    ScenarioResult textLigatureDirtyRow(int warmupIterations,
                                        int measuredIterations,
                                        RenderDocCapture *capture,
                                        QString *error)
    {
        return textShapingScenario(QStringLiteral("text-ligature-dirty-row"),
                                   u"ab>=cd===ef=", u"ij===kl>=mn=", false,
                                   false, warmupIterations, measuredIterations,
                                   capture, error);
    }

    ScenarioResult textAsciiFullFrame(int warmupIterations,
                                      int measuredIterations,
                                      RenderDocCapture *capture, QString *error)
    {
        return textShapingScenario(QStringLiteral("text-ascii-full-frame"),
                                   u"ab+cd=ef+gh-", u"ij-kl+mn=op+", true, true,
                                   warmupIterations, measuredIterations,
                                   capture, error);
    }

    ScenarioResult textLigatureFullFrame(int warmupIterations,
                                         int measuredIterations,
                                         RenderDocCapture *capture,
                                         QString *error)
    {
        return textShapingScenario(QStringLiteral("text-ligature-full-frame"),
                                   u"ab>=cd===ef=", u"ij===kl>=mn=", true,
                                   false, warmupIterations, measuredIterations,
                                   capture, error);
    }

    ScenarioResult textAtlasRetainedRebuild(int warmupIterations,
                                            int measuredIterations,
                                            RenderDocCapture *capture,
                                            QString *error)
    {
        clearSearch();
        const QStringView pattern = u"ab+cd=ef+gh-";

        // Establish this scenario's own row topology. The second palette is a
        // guaranteed text-state transition regardless of state inherited from
        // a previously measured scenario.
        publishGlobalDependentTextFrame(pattern, false);
        if (!renderUntimed(error)) return {};
        publishTextPalette(true);
        if (!renderUntimed(error)) return {};

        const TerminalPaneRenderProbeSnapshot baseline =
            terminalPaneRenderProbe(pane_);
        const bool expectBatch = glyphBatchExpected();
        const quint64 rows = static_cast<quint64>(grid_.rows);
        const quint64 cells = static_cast<quint64>(grid_.columns) * rows;
        bool alternatePalette = true;
        ScenarioResult result = measure(
            QStringLiteral("text-atlas-retained-rebuild"), warmupIterations,
            measuredIterations,
            {.paintSerial = 1,
             .solidCellVisits = cells,
             .textRowBuilds = rows,
             .textLayouts = expectBatch ? 0 : rows,
             .nativeTextSubmissions = expectBatch ? 0 : rows,
             .nativeTextCells = expectBatch ? 0 : cells,
             .batchedGlyphs = expectBatch ? cells : 0,
             .glyphBatchGeometryWrites = expectBatch ? rows : 0,
             .glyphBatchNodeCreations = 0,
             .glyphBatchAllocations = 0},
            0, 0, {},
            [this, &alternatePalette] {
                alternatePalette = !alternatePalette;
                publishTextPalette(alternatePalette);
            },
            capture, error);
        if (result.name.isEmpty()) return result;

        result.expectedFinalNativeTextNodeCount = baseline.nativeTextNodeCount;
        result.expectedFinalGlyphAtlasSerial = baseline.glyphAtlasSerial;
        result.expectedFinalRowContainerSerials = baseline.rowContainerSerials;
        result.expectedFinalRowGlyphBatchSerials =
            baseline.rowGlyphBatchSerials;
        result.expectedFinalRowNativeTextNodeSerials = baseline.rowNodeSerials;

        return result;
    }

    ScenarioResult textExplicitColorGlobalNoop(int warmupIterations,
                                               int measuredIterations,
                                               RenderDocCapture *capture,
                                               QString *error)
    {
        clearSearch();
        const QStringView pattern = u"ab+cd=ef+gh-";

        publishDependencyTextFrame(pattern, false, false);
        if (!renderUntimed(error)) return {};
        publishGlobalTextColors(true);
        if (!renderUntimed(error)) return {};

        const TerminalPaneRenderProbeSnapshot baseline =
            terminalPaneRenderProbe(pane_);
        bool alternateColors = true;
        ScenarioResult result = measure(
            QStringLiteral("text-explicit-color-global-noop"), warmupIterations,
            measuredIterations, {.paintSerial = 1}, 0, 0, {},
            [this, &alternateColors] {
                alternateColors = !alternateColors;
                publishGlobalTextColors(alternateColors);
            },
            capture, error);
        if (result.name.isEmpty()) return result;

        requireRetainedTextResources(result, baseline);
        return result;
    }

    ScenarioResult textSelectiveColorChange(int warmupIterations,
                                            int measuredIterations,
                                            RenderDocCapture *capture,
                                            QString *error)
    {
        clearSearch();
        const QStringView pattern = u"ab+cd=ef+gh-";

        publishDependencyTextFrame(pattern, false, true);
        if (!renderUntimed(error)) return {};
        publishGlobalTextColors(true);
        if (!renderUntimed(error)) return {};

        const TerminalPaneRenderProbeSnapshot baseline =
            terminalPaneRenderProbe(pane_);
        const bool expectBatch = glyphBatchExpected();
        const quint64 rows = static_cast<quint64>((grid_.rows + 1) / 2);
        const quint64 cells = static_cast<quint64>(grid_.columns) * rows;
        bool alternateColors = true;
        ScenarioResult result = measure(
            QStringLiteral("text-selective-color-change"), warmupIterations,
            measuredIterations,
            {.paintSerial = 1,
             .solidCellVisits = cells,
             .textRowBuilds = rows,
             .textLayouts = expectBatch ? 0 : rows,
             .nativeTextSubmissions = expectBatch ? 0 : rows,
             .nativeTextCells = expectBatch ? 0 : cells,
             .batchedGlyphs = expectBatch ? cells : 0,
             .glyphBatchGeometryWrites = expectBatch ? rows : 0,
             .glyphBatchNodeCreations = 0,
             .glyphBatchAllocations = 0},
            0, 0, {},
            [this, &alternateColors] {
                alternateColors = !alternateColors;
                publishGlobalTextColors(alternateColors);
            },
            capture, error);
        if (result.name.isEmpty()) return result;

        requireRetainedTextResources(result, baseline);
        return result;
    }

    ScenarioResult cursorOnly(int warmupIterations, int measuredIterations,
                              RenderDocCapture *capture, QString *error)
    {
        if (!prepareNativeControlFrame(error)) return {};
        cursorRow_ = 0;
        publishCursor();
        if (!renderUntimed(error)) return {};

        const quint64 nativeCells = 2 * nativeControlCellsPerRow();

        return measure(
            QStringLiteral("cursor-only"), warmupIterations, measuredIterations,
            {.paintSerial = 1,
             .solidCellVisits = static_cast<quint64>(2 * grid_.columns),
             .textRowBuilds = 2,
             .textLayouts = 3,
             .nativeTextSubmissions = 3,
             .nativeTextCells = nativeCells},
            0, 0, {},
            [this] {
                cursorRow_ = cursorRow_ == 0 ? grid_.rows - 1 : 0;
                publishCursor();
            },
            capture, error);
    }

    ScenarioResult fullInvalidation(int warmupIterations,
                                    int measuredIterations,
                                    RenderDocCapture *capture, QString *error)
    {
        bool odd = false;
        return measure(
            QStringLiteral("full-invalidation"), warmupIterations,
            measuredIterations,
            {.paintSerial = 1,
             .solidCellVisits =
                 static_cast<quint64>(grid_.columns * grid_.rows),
             .textRowBuilds = static_cast<quint64>(grid_.rows),
             .textLayouts = static_cast<quint64>(grid_.rows),
             .nativeTextSubmissions = static_cast<quint64>(grid_.rows),
             .nativeTextCells =
                 nativeControlCellsPerRow() * static_cast<quint64>(grid_.rows)},
            0, 0, {},
            [this, &odd] {
                odd = !odd;
                publishFullFrame(odd);
            },
            capture, error);
    }

    ScenarioResult searchUpdate(int warmupIterations, int measuredIterations,
                                RenderDocCapture *capture, QString *error)
    {
        if (!prepareNativeControlFrame(error)) return {};
        publishSearch(0);
        if (!renderUntimed(error)) return {};
        int row = 0;
        return measure(
            QStringLiteral("search-update"), warmupIterations,
            measuredIterations,
            {.paintSerial = 1,
             .solidCellVisits = static_cast<quint64>(2 * grid_.columns),
             .textRowBuilds = 2,
             .textLayouts = 3,
             .nativeTextSubmissions = 3,
             .nativeTextCells = 2 * nativeControlCellsPerRow()},
            0, 0, {},
            [this, &row] {
                row = row == 1 ? 2 : 1;
                publishSearch(row);
            },
            capture, error);
    }

    ScenarioResult searchSelectionUpdate(int warmupIterations,
                                         int measuredIterations,
                                         RenderDocCapture *capture,
                                         QString *error)
    {
        if (!prepareNativeControlFrame(error)) return {};
        constexpr int row = 1;
        publishSearch(row);
        if (!renderUntimed(error)) return {};
        bool selected = false;
        return measure(
            QStringLiteral("search-selection-update"), warmupIterations,
            measuredIterations,
            {.paintSerial = 1,
             .solidCellVisits = static_cast<quint64>(grid_.columns),
             .textRowBuilds = 1,
             .textLayouts = 2,
             .nativeTextSubmissions = 2,
             .nativeTextCells = nativeControlCellsPerRow()},
            0, 0, {},
            [this, &selected] {
                selected = !selected;
                publishSearch(row, selected);
            },
            capture, error);
    }

    ScenarioResult searchClear(int warmupIterations, int measuredIterations,
                               RenderDocCapture *capture, QString *error)
    {
        if (!prepareNativeControlFrame(error)) return {};
        constexpr int row = 1;
        return measure(
            QStringLiteral("search-clear"), warmupIterations,
            measuredIterations,
            {.paintSerial = 1,
             .solidCellVisits = static_cast<quint64>(grid_.columns),
             .textRowBuilds = 1,
             .textLayouts = 1,
             .nativeTextSubmissions = 1,
             .nativeTextCells = nativeControlCellsPerRow()},
            0, 0,
            [this, error] {
                publishSearch(row);
                return renderUntimed(error);
            },
            [this] { clearSearch(); }, capture, error);
    }

    ScenarioResult kittyFirstUpload(int warmupIterations,
                                    int measuredIterations,
                                    RenderDocCapture *capture, QString *error)
    {
        return kittyFirstUploadScenario(QStringLiteral("kitty-first-upload"),
                                        255, warmupIterations,
                                        measuredIterations, capture, error);
    }

    ScenarioResult kittyTranslucentFirstUpload(int warmupIterations,
                                               int measuredIterations,
                                               RenderDocCapture *capture,
                                               QString *error)
    {
        return kittyFirstUploadScenario(
            QStringLiteral("kitty-translucent-first-upload"), 128,
            warmupIterations, measuredIterations, capture, error);
    }

    ScenarioResult kittyFirstUploadScenario(const QString &name, int opacity,
                                            int warmupIterations,
                                            int measuredIterations,
                                            RenderDocCapture *capture,
                                            QString *error)
    {
        if (!prepareNativeControlFrame(error)) return {};
        quint64 generation = ++kittyGeneration_;
        std::shared_ptr<const TerminalKittyGraphicsSnapshot> nextSnapshot;
        return measure(
            name, warmupIterations, measuredIterations,
            {
                .paintSerial = 1,
                .kittyTextureUploads = 1,
                .kittyNodeCreations = kittyPlacementCount,
                .kittyGeometryWrites = kittyPlacementCount,
                .kittyMaterialAssignments = kittyPlacementCount,
                .kittyTextureSetCount = 1,
            },
            1, kittyTextureLogicalBytes,
            [this, &generation, &nextSnapshot, opacity, error] {
                publishKitty(nullptr);
                if (!renderUntimed(error)) return false;
                nextSnapshot = makeKittySnapshot(
                    makeKittyImage(++generation, QColor(32, 96, 192), opacity),
                    0);
                return true;
            },
            [this, &nextSnapshot] { publishKitty(nextSnapshot); }, capture,
            error);
    }

    ScenarioResult kittyRetainedRedraw(int warmupIterations,
                                       int measuredIterations,
                                       RenderDocCapture *capture,
                                       QString *error)
    {
        return kittyRetainedRedrawScenario(
            QStringLiteral("kitty-retained-redraw"), 255, warmupIterations,
            measuredIterations, capture, error);
    }

    ScenarioResult kittyTranslucentRetainedRedraw(int warmupIterations,
                                                  int measuredIterations,
                                                  RenderDocCapture *capture,
                                                  QString *error)
    {
        return kittyRetainedRedrawScenario(
            QStringLiteral("kitty-translucent-retained-redraw"), 128,
            warmupIterations, measuredIterations, capture, error);
    }

    ScenarioResult kittyRetainedRedrawScenario(const QString &name, int opacity,
                                               int warmupIterations,
                                               int measuredIterations,
                                               RenderDocCapture *capture,
                                               QString *error)
    {
        if (!prepareNativeControlFrame(error)) return {};
        const auto snapshot = makeKittySnapshot(
            makeKittyImage(++kittyGeneration_, QColor(48, 144, 208), opacity),
            0);
        publishKitty(snapshot);
        if (!renderUntimed(error)) return {};
        return measure(
            name, warmupIterations, measuredIterations, {.paintSerial = 1}, 1,
            kittyTextureLogicalBytes, {},
            [this, snapshot] { publishKitty(snapshot); }, capture, error);
    }

    ScenarioResult kittyMovement(int warmupIterations, int measuredIterations,
                                 RenderDocCapture *capture, QString *error)
    {
        if (!prepareNativeControlFrame(error)) return {};
        const auto asset =
            makeKittyImage(++kittyGeneration_, QColor(64, 176, 112));
        const auto left = makeKittySnapshot(asset, 0);
        const auto right = makeKittySnapshot(asset, 1);
        publishKitty(left);
        if (!renderUntimed(error)) return {};
        bool moved = false;
        return measure(
            QStringLiteral("kitty-movement"), warmupIterations,
            measuredIterations,
            {
                .paintSerial = 1,
                .kittyGeometryWrites = kittyPlacementCount,
            },
            1, kittyTextureLogicalBytes, {},
            [this, left, right, &moved] {
                moved = !moved;
                publishKitty(moved ? right : left);
            },
            capture, error);
    }

    ScenarioResult kittyReplacement(int warmupIterations,
                                    int measuredIterations,
                                    RenderDocCapture *capture, QString *error)
    {
        return kittyReplacementScenario(QStringLiteral("kitty-replacement"),
                                        255, warmupIterations,
                                        measuredIterations, capture, error);
    }

    ScenarioResult kittyTranslucentReplacement(int warmupIterations,
                                               int measuredIterations,
                                               RenderDocCapture *capture,
                                               QString *error)
    {
        return kittyReplacementScenario(
            QStringLiteral("kitty-translucent-replacement"), 128,
            warmupIterations, measuredIterations, capture, error);
    }

    ScenarioResult kittyReplacementScenario(const QString &name, int opacity,
                                            int warmupIterations,
                                            int measuredIterations,
                                            RenderDocCapture *capture,
                                            QString *error)
    {
        if (!prepareNativeControlFrame(error)) return {};
        const auto first = makeKittySnapshot(
            makeKittyImage(++kittyGeneration_, QColor(224, 96, 48), opacity),
            0);
        const auto second = makeKittySnapshot(
            makeKittyImage(++kittyGeneration_, QColor(240, 192, 48), opacity),
            0);
        publishKitty(first);
        if (!renderUntimed(error)) return {};
        bool replaced = false;
        return measure(
            name, warmupIterations, measuredIterations,
            {
                .paintSerial = 1,
                .kittyTextureUploads = 1,
                .kittyMaterialAssignments = kittyPlacementCount,
                .kittyTextureSetEvictions = 1,
            },
            1, kittyTextureLogicalBytes, {},
            [this, first, second, &replaced] {
                replaced = !replaced;
                publishKitty(replaced ? second : first);
            },
            capture, error);
    }

    ScenarioResult kittyEviction(int warmupIterations, int measuredIterations,
                                 RenderDocCapture *capture, QString *error)
    {
        if (!prepareNativeControlFrame(error)) return {};
        quint64 generation = ++kittyGeneration_;
        return measure(
            QStringLiteral("kitty-eviction"), warmupIterations,
            measuredIterations,
            {
                .paintSerial = 1,
                .kittyNodeDeletions = kittyPlacementCount,
                .kittyTextureSetEvictions = 1,
                .kittyTextureSetCount = -1,
            },
            0, 0,
            [this, &generation, error] {
                publishKitty(makeKittySnapshot(
                    makeKittyImage(++generation, QColor(128, 64, 192)), 0));
                return renderUntimed(error);
            },
            [this] { publishKitty(nullptr); }, capture, error);
    }

private:
    [[nodiscard]] quint64 nativeControlCellsPerRow() const noexcept
    {
        constexpr int glyphInterval = 16;
        return static_cast<quint64>((grid_.columns + glyphInterval - 1)
                                    / glyphInterval);
    }

    bool prepareNativeControlFrame(QString *error)
    {
        clearSearch();
        publishFullFrame(false);
        publishKitty(nullptr);
        return renderUntimed(error);
    }

    bool glyphBatchExpected() const
    {
        bool disabledParsed = false;
        const int disabledValue = qEnvironmentVariableIntValue(
            "GHOSTTY_QT_DISABLE_GLYPH_BATCH", &disabledParsed);
        const bool batchDisabled = disabledParsed
            ? disabledValue != 0
            : qEnvironmentVariableIsSet("GHOSTTY_QT_DISABLE_GLYPH_BATCH");
        return graphicsApi_ != GraphicsApi::Software && !batchDisabled;
    }

    ScenarioResult
    textShapingScenario(const QString &name, QStringView firstPattern,
                        QStringView secondPattern, bool fullFrame,
                        bool batchCandidate, int warmupIterations,
                        int measuredIterations, RenderDocCapture *capture,
                        QString *error)
    {
        Q_ASSERT(firstPattern.size() == secondPattern.size());
        bool alternate = false;
        const quint64 rebuiltRows =
            static_cast<quint64>(fullFrame ? grid_.rows : 1);
        const bool expectBatch = batchCandidate && glyphBatchExpected();
        const quint64 cells = static_cast<quint64>(grid_.columns) * rebuiltRows;
        return measure(
            name, warmupIterations, measuredIterations,
            {.paintSerial = 1,
             .solidCellVisits =
                 static_cast<quint64>(grid_.columns) * rebuiltRows,
             .textRowBuilds = rebuiltRows,
             .textLayouts = expectBatch ? 0 : rebuiltRows,
             .nativeTextSubmissions = expectBatch ? 0 : rebuiltRows,
             .nativeTextCells = expectBatch ? 0 : cells,
             .batchedGlyphs = expectBatch ? cells : 0,
             .glyphBatchGeometryWrites = expectBatch ? rebuiltRows : 0},
            0, 0, {},
            [this, firstPattern, secondPattern, fullFrame, &alternate] {
                alternate = !alternate;
                const QStringView pattern =
                    alternate ? secondPattern : firstPattern;
                if (fullFrame) {
                    publishTextFrame(pattern);
                } else {
                    publishTextRow(pattern);
                }
            },
            capture, error);
    }

    InitializationResult initializeRhi(QString *error)
    {
#if GHOSTTY_QT_PANE_BENCH_HAS_VULKAN
        if (graphicsApi_ == GraphicsApi::Vulkan) {
            vulkanInstance_ = std::make_unique<QVulkanInstance>();
            vulkanInstance_->setExtensions(
                QQuickGraphicsConfiguration::preferredInstanceExtensions());
            if (!vulkanInstance_->create()) {
                *error = QStringLiteral(
                             "unable to create Vulkan instance (VkResult %1)")
                             .arg(vulkanInstance_->errorCode());
                return InitializationResult::BackendUnavailable;
            }
            window_->setVulkanInstance(vulkanInstance_.get());
        }
#endif
        if (!renderControl_->initialize()) {
            *error = QStringLiteral(
                         "QQuickRenderControl could not initialize %1 on %2")
                         .arg(graphicsApiName(graphicsApi_))
                         .arg(QGuiApplication::platformName());
            return InitializationResult::BackendUnavailable;
        }
        const QSGRendererInterface::GraphicsApi expectedApi =
            graphicsApi_ == GraphicsApi::OpenGl ? QSGRendererInterface::OpenGL
                                                : QSGRendererInterface::Vulkan;
        if (window_->rendererInterface()->graphicsApi() != expectedApi) {
            *error =
                QStringLiteral("requested %1, but Qt selected graphics API %2")
                    .arg(graphicsApiName(graphicsApi_))
                    .arg(static_cast<int>(
                        window_->rendererInterface()->graphicsApi()));
            return InitializationResult::Failure;
        }
        QRhi *const rhi = renderControl_->rhi();
        if (rhi == nullptr) {
            *error =
                QStringLiteral("QQuickRenderControl did not provide a QRhi");
            return InitializationResult::Failure;
        }
        driverInfo_ = rhi->driverInfo();
        const qreal dpr = window_->devicePixelRatio();
        const QSize physicalSize(
            qMax(1, qRound(static_cast<qreal>(logicalSize_.width()) * dpr)),
            qMax(1, qRound(static_cast<qreal>(logicalSize_.height()) * dpr)));
        colorBuffer_.reset(rhi->newTexture(
            QRhiTexture::RGBA8, physicalSize, 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        colorBuffer_->setName(
            QByteArrayLiteral("ghostty-qt terminal-pane benchmark output"));
        framebufferSize_ = physicalSize;
        if (!colorBuffer_->create()) {
            *error =
                QStringLiteral("unable to create pane benchmark color buffer");
            return InitializationResult::Failure;
        }
        depthStencil_.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil,
                                                 physicalSize, 1));
        depthStencil_->setName(QByteArrayLiteral(
            "ghostty-qt terminal-pane benchmark depth-stencil"));
        if (!depthStencil_->create()) {
            *error = QStringLiteral(
                "unable to create pane benchmark depth-stencil buffer");
            return InitializationResult::Failure;
        }
        const QRhiTextureRenderTargetDescription description(
            QRhiColorAttachment(colorBuffer_.get()), depthStencil_.get());
        renderTarget_.reset(rhi->newTextureRenderTarget(description));
        renderPassDescriptor_.reset(
            renderTarget_->newCompatibleRenderPassDescriptor());
        renderPassDescriptor_->setName(
            QByteArrayLiteral("ghostty-qt terminal-pane benchmark pass"));
        renderTarget_->setRenderPassDescriptor(renderPassDescriptor_.get());
        renderTarget_->setName(
            QByteArrayLiteral("ghostty-qt terminal-pane benchmark target"));
        if (!renderTarget_->create()) {
            *error =
                QStringLiteral("unable to create pane benchmark render target");
            return InitializationResult::Failure;
        }
        QQuickRenderTarget quickTarget =
            QQuickRenderTarget::fromRhiRenderTarget(renderTarget_.get());
        quickTarget.setDevicePixelRatio(dpr);
        window_->setRenderTarget(quickTarget);
        return InitializationResult::Success;
    }

    TerminalRowUpdate makeRow(int row, const QColor &background) const
    {
        TerminalRowUpdate update;
        update.row = row;
        update.cells.resize(grid_.columns);
        for (int column = 0; column < grid_.columns; ++column) {
            TerminalCell &cell = update.cells[column];
            cell.foreground = QColor(QStringLiteral("#d8dee9"));
            cell.background = background;
            if (column % 16 == 0) cell.text = QString(QChar(0x2588));
        }
        return update;
    }

    TerminalRowUpdate makeTextRow(int row, QStringView pattern) const
    {
        Q_ASSERT(!pattern.isEmpty());
        TerminalRowUpdate update;
        update.row = row;
        update.cells.reserve(grid_.columns);
        for (int column = 0; column < grid_.columns; ++column) {
            const QChar character = pattern.at(column % pattern.size());
            TerminalCell cell;
            cell.text = QString(character);
            cell.baseCodepoint = character.unicode();
            cell.setPlainCodepoint(true);
            cell.foreground = QColor(QStringLiteral("#d8dee9"));
            cell.background = Qt::black;
            update.cells.append(std::move(cell));
        }
        return update;
    }

    TerminalRowUpdate makeExplicitColorTextRow(int row,
                                               QStringView pattern) const
    {
        TerminalRowUpdate update = makeTextRow(row, pattern);
        for (TerminalCell &cell : update.cells) {
            cell.foreground = QColor(QStringLiteral("#91d7e3"));
            cell.background = QColor(QStringLiteral("#1b2430"));
            cell.setStyleForegroundSource(TerminalColorSource::Rgb);
            cell.setBackgroundExplicit(true);
        }
        return update;
    }

    TerminalRowUpdate makeGlobalColorDependentTextRow(int row,
                                                      QStringView pattern,
                                                      bool globalVariant) const
    {
        TerminalRowUpdate update = makeTextRow(row, pattern);
        const bool paletteDerived = row % 4 == 0;
        const QColor foreground = paletteDerived
            ? textPalette(globalVariant).at(0)
            : globalVariant ? QColor(QStringLiteral("#eceff4"))
                            : QColor(QStringLiteral("#d8dee9"));
        for (TerminalCell &cell : update.cells) {
            cell.foreground = foreground;
            cell.background = QColor(QStringLiteral("#1b2430"));
            cell.setBackgroundExplicit(true);
            cell.setStyleForegroundSource(paletteDerived
                                              ? TerminalColorSource::Palette
                                              : TerminalColorSource::Rgb);
            cell.setStyleForegroundPaletteIndex(paletteDerived ? 0 : -1);
            cell.setBold(true);
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
        update.cursorChanged = true;
        update.cursorVisible = false;
        update.cursorBlinking = false;
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

    void publishTextRow(QStringView pattern)
    {
        TerminalUpdate update;
        update.columns = grid_.columns;
        update.rows = grid_.rows;
        update.dirtyRows.append(makeTextRow(grid_.rows / 2, pattern));
        update.contentRevision = ++revision_;
        controller_->terminalUpdated(update);
    }

    QVector<QColor> textPalette(bool alternate) const
    {
        QVector<QColor> result;
        result.reserve(16);
        for (int index = 0; index < 16; ++index) {
            const int offset = alternate ? 29 : 0;
            result.append(QColor::fromRgb((index * 47 + offset) & 0xff,
                                          (index * 83 + offset) & 0xff,
                                          (index * 131 + offset) & 0xff));
        }
        return result;
    }

    void publishTextFrame(QStringView pattern,
                          std::optional<bool> paletteVariant = std::nullopt)
    {
        TerminalUpdate update;
        update.columns = grid_.columns;
        update.rows = grid_.rows;
        update.fullFrame = true;
        update.foreground = paletteVariant.value_or(false)
            ? QColor(QStringLiteral("#eceff4"))
            : QColor(QStringLiteral("#d8dee9"));
        update.background = Qt::black;
        update.cursorColor = QColor(QStringLiteral("#f0f0f0"));
        update.cursorColorExplicit = true;
        if (paletteVariant.has_value()) {
            update.palette = textPalette(*paletteVariant);
        }
        update.contentRevision = ++revision_;
        update.dirtyRows.reserve(grid_.rows);
        for (int row = 0; row < grid_.rows; ++row) {
            update.dirtyRows.append(makeTextRow(row, pattern));
        }
        controller_->terminalUpdated(update);
    }

    void publishTextPalette(bool alternate)
    {
        TerminalUpdate update;
        update.columns = grid_.columns;
        update.rows = grid_.rows;
        update.colorsChanged = true;
        update.foreground = alternate ? QColor(QStringLiteral("#eceff4"))
                                      : QColor(QStringLiteral("#d8dee9"));
        update.background = Qt::black;
        update.cursorColor = QColor(QStringLiteral("#f0f0f0"));
        update.cursorColorExplicit = true;
        update.palette = textPalette(alternate);
        update.contentRevision = ++revision_;
        controller_->terminalUpdated(update);
    }

    void publishDependencyTextFrame(QStringView pattern, bool globalVariant,
                                    bool mixedDependencies)
    {
        TerminalUpdate update;
        update.columns = grid_.columns;
        update.rows = grid_.rows;
        update.fullFrame = true;
        update.foreground = globalVariant ? QColor(QStringLiteral("#eceff4"))
                                          : QColor(QStringLiteral("#d8dee9"));
        update.background = globalVariant ? QColor(QStringLiteral("#14191f"))
                                          : QColor(QStringLiteral("#090c10"));
        update.cursorColor = QColor(QStringLiteral("#f0f0f0"));
        update.cursorColorExplicit = true;
        update.palette = textPalette(globalVariant);
        update.contentRevision = ++revision_;
        update.dirtyRows.reserve(grid_.rows);
        for (int row = 0; row < grid_.rows; ++row) {
            const bool globalColorDependent = mixedDependencies && row % 2 == 0;
            update.dirtyRows.append(
                globalColorDependent ? makeGlobalColorDependentTextRow(
                                           row, pattern, globalVariant)
                                     : makeExplicitColorTextRow(row, pattern));
        }
        controller_->terminalUpdated(update);
    }

    void publishGlobalDependentTextFrame(QStringView pattern,
                                         bool globalVariant)
    {
        TerminalUpdate update;
        update.columns = grid_.columns;
        update.rows = grid_.rows;
        update.fullFrame = true;
        update.foreground = globalVariant ? QColor(QStringLiteral("#eceff4"))
                                          : QColor(QStringLiteral("#d8dee9"));
        update.background = Qt::black;
        update.cursorColor = QColor(QStringLiteral("#f0f0f0"));
        update.cursorColorExplicit = true;
        update.palette = textPalette(globalVariant);
        update.contentRevision = ++revision_;
        update.dirtyRows.reserve(grid_.rows);
        for (int row = 0; row < grid_.rows; ++row) {
            update.dirtyRows.append(
                makeGlobalColorDependentTextRow(row, pattern, globalVariant));
        }
        controller_->terminalUpdated(update);
    }

    void publishGlobalTextColors(bool alternate)
    {
        TerminalUpdate update;
        update.columns = grid_.columns;
        update.rows = grid_.rows;
        update.colorsChanged = true;
        update.foreground = alternate ? QColor(QStringLiteral("#eceff4"))
                                      : QColor(QStringLiteral("#d8dee9"));
        update.background = alternate ? QColor(QStringLiteral("#14191f"))
                                      : QColor(QStringLiteral("#090c10"));
        update.cursorColor = QColor(QStringLiteral("#f0f0f0"));
        update.cursorColorExplicit = true;
        update.palette = textPalette(alternate);
        update.contentRevision = ++revision_;
        controller_->terminalUpdated(update);
    }

    static void requireRetainedTextResources(
        ScenarioResult &result, const TerminalPaneRenderProbeSnapshot &baseline)
    {
        result.expectedFinalNativeTextNodeCount = baseline.nativeTextNodeCount;
        result.expectedFinalGlyphAtlasSerial = baseline.glyphAtlasSerial;
        result.expectedFinalRowContainerSerials = baseline.rowContainerSerials;
        result.expectedFinalRowGlyphBatchSerials =
            baseline.rowGlyphBatchSerials;
        result.expectedFinalRowNativeTextNodeSerials = baseline.rowNodeSerials;
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

    void publishSearch(int row, bool selected = false)
    {
        TerminalSearchUpdate search;
        search.generation = ++searchGeneration_;
        search.contentRevision = revision_;
        search.active = true;
        search.complete = true;
        search.scannedRows = static_cast<quint64>(grid_.rows);
        search.totalRows = static_cast<quint64>(grid_.rows);
        search.totalMatches = 1;
        search.selectedMatch = selected ? 0 : -1;
        search.columns = grid_.columns;
        search.rows = grid_.rows;
        const qsizetype cellCount =
            static_cast<qsizetype>(grid_.columns) * grid_.rows;
        search.visibleCellMask = QBitArray(cellCount);
        search.selectedCellMask = QBitArray(cellCount);
        const qsizetype index =
            static_cast<qsizetype>(std::clamp(row, 0, grid_.rows - 1))
                * grid_.columns
            + grid_.columns / 2;
        search.visibleCellMask.setBit(index);
        if (selected) search.selectedCellMask.setBit(index);
        controller_->searchUpdated(search);
    }

    void clearSearch()
    {
        TerminalSearchUpdate search;
        search.generation = ++searchGeneration_;
        search.contentRevision = revision_;
        controller_->searchUpdated(search);
    }

    std::shared_ptr<const TerminalKittyGraphicsImage>
    makeKittyImage(quint64 generation, const QColor &color,
                   int opacity = 255) const
    {
        QImage rgba(QSize(64, 64), QImage::Format_RGBA8888);
        QColor packed = color;
        packed.setAlpha(opacity);
        rgba.fill(packed);
        return std::make_shared<const TerminalKittyGraphicsImage>(
            TerminalKittyGraphicsImage{
                .imageId = 42,
                .generation = generation,
                .fullyOpaque = opacity == 255,
                .straightRgba = std::move(rgba),
            });
    }

    std::shared_ptr<const TerminalKittyGraphicsSnapshot> makeKittySnapshot(
        const std::shared_ptr<const TerminalKittyGraphicsImage> &asset,
        int columnOffset, int placementCount = kittyPlacementCount) const
    {
        auto snapshot = std::make_shared<TerminalKittyGraphicsSnapshot>();
        snapshot->storageGeneration = asset->generation;
        const TerminalPaneRenderProbeSnapshot probe =
            terminalPaneRenderProbe(pane_);
        const qreal dpr = window_->devicePixelRatio();
        snapshot->cellWidthPixels = static_cast<quint32>(
            qMax(1, qRound(probe.metrics.cellWidth * dpr)));
        snapshot->cellHeightPixels = static_cast<quint32>(
            qMax(1, qRound(probe.metrics.cellHeight * dpr)));
        snapshot->placements.reserve(placementCount);
        for (int index = 0; index < placementCount; ++index) {
            const int column =
                (index % qMax(1, grid_.columns - 1)) + columnOffset;
            const int row = (index / qMax(1, grid_.columns - 1)) % grid_.rows;
            snapshot->placements.append({
                .image = asset,
                .placementId = static_cast<quint32>(index + 1),
                .z = 1,
                .layer = TerminalKittyGraphicsLayer::AboveText,
                .viewportColumn = column,
                .viewportRow = row,
                .destinationWidthPixels = snapshot->cellWidthPixels,
                .destinationHeightPixels = snapshot->cellHeightPixels,
                .sourceWidth =
                    static_cast<quint32>(asset->straightRgba.width()),
                .sourceHeight =
                    static_cast<quint32>(asset->straightRgba.height()),
            });
        }
        return snapshot;
    }

    void publishKitty(
        const std::shared_ptr<const TerminalKittyGraphicsSnapshot> &snapshot)
    {
        auto empty = std::make_shared<TerminalKittyGraphicsSnapshot>();
        if (snapshot == nullptr) {
            empty->storageGeneration = ++kittyGeneration_;
            const TerminalPaneRenderProbeSnapshot probe =
                terminalPaneRenderProbe(pane_);
            const qreal dpr = window_->devicePixelRatio();
            empty->cellWidthPixels = static_cast<quint32>(
                qMax(1, qRound(probe.metrics.cellWidth * dpr)));
            empty->cellHeightPixels = static_cast<quint32>(
                qMax(1, qRound(probe.metrics.cellHeight * dpr)));
        }
        TerminalUpdate update;
        update.columns = grid_.columns;
        update.rows = grid_.rows;
        update.kittyGraphicsChanged = true;
        update.kittyGraphics = snapshot != nullptr ? snapshot : empty;
        update.contentRevision = ++revision_;
        controller_->terminalUpdated(update);
    }

    bool renderUntimed(QString *error, bool validateOutput = false,
                       const QColor &expectedColor = {})
    {
        FrameTiming ignored;
        return renderFrame({}, &ignored, error, validateOutput, expectedColor);
    }

    bool renderFrame(const std::function<void()> &prepare, FrameTiming *timing,
                     QString *error, bool validateOutput = false,
                     const QColor &expectedColor = {})
    {
        QElapsedTimer totalTimer;
        totalTimer.start();
        QElapsedTimer updateTimer;
        updateTimer.start();
        if (prepare) prepare();
        timing->cpuUpdateNanoseconds = updateTimer.nsecsElapsed();

        if (renderControl_ == nullptr) {
            const QImage image = window_->grabWindow();
            timing->cpuTotalNanoseconds = totalTimer.nsecsElapsed();
            if (image.isNull()) {
                *error = QStringLiteral(
                    "software scene-graph grab returned no image");
                return false;
            }
            if (validateOutput && !containsNonBlackPixel(image)) {
                *error = QStringLiteral(
                    "software scene-graph validation rendered only black");
                return false;
            }
            if (expectedColor.isValid()
                && !containsColorPixel(image, expectedColor)) {
                *error = QStringLiteral(
                             "software scene-graph validation did not render "
                             "expected color %1")
                             .arg(expectedColor.name());
                return false;
            }
            return true;
        }

        QRhiReadbackResult readback;
        QElapsedTimer recordTimer;
        recordTimer.start();
        renderControl_->polishItems();
        renderControl_->beginFrame();
        renderControl_->sync();
        renderControl_->render();
        QRhiCommandBuffer *const commandBuffer =
            renderControl_->commandBuffer();
        if (commandBuffer == nullptr) {
            renderControl_->endFrame();
            *error = QStringLiteral(
                "QQuickRenderControl did not provide a command buffer");
            return false;
        }
        if (validateOutput) {
            QRhiResourceUpdateBatch *const readbackBatch =
                renderControl_->rhi()->nextResourceUpdateBatch();
            readbackBatch->readBackTexture(colorBuffer_.get(), &readback);
            commandBuffer->resourceUpdate(readbackBatch);
        }
        timing->cpuRecordNanoseconds = recordTimer.nsecsElapsed();

        QElapsedTimer completionTimer;
        completionTimer.start();
        renderControl_->endFrame();
        timing->cpuCompletionNanoseconds = completionTimer.nsecsElapsed();
        timing->cpuTotalNanoseconds = *timing->cpuUpdateNanoseconds
            + *timing->cpuRecordNanoseconds + *timing->cpuCompletionNanoseconds;
        QRhi *const rhi = renderControl_->rhi();
        if (rhi->isFeatureSupported(QRhi::Timestamps)) {
            const double gpuSeconds = commandBuffer->lastCompletedGpuTime();
            if (std::isfinite(gpuSeconds) && gpuSeconds > 0.0) {
                timing->gpuNanoseconds = qRound64(gpuSeconds * 1'000'000'000.0);
            }
        }
        if (validateOutput) {
            if (readback.data.isEmpty() || readback.pixelSize.isEmpty()) {
                *error = QStringLiteral(
                    "offscreen texture validation returned no pixels");
                return false;
            }
            const qsizetype expectedBytes =
                static_cast<qsizetype>(readback.pixelSize.width())
                * readback.pixelSize.height() * 4;
            if (readback.data.size() < expectedBytes) {
                *error = QStringLiteral(
                             "offscreen texture validation returned %1 bytes; "
                             "expected at least %2")
                             .arg(readback.data.size())
                             .arg(expectedBytes);
                return false;
            }
            const QImage image(
                reinterpret_cast<const uchar *>(readback.data.constData()),
                readback.pixelSize.width(), readback.pixelSize.height(),
                QImage::Format_RGBA8888_Premultiplied);
            if (!containsNonBlackPixel(image)) {
                *error = QStringLiteral(
                    "offscreen texture validation rendered only black");
                return false;
            }
            if (expectedColor.isValid()
                && !containsColorPixel(image, expectedColor)) {
                *error = QStringLiteral(
                             "offscreen texture validation did not render "
                             "expected color %1")
                             .arg(expectedColor.name());
                return false;
            }
        }
        return true;
    }

    ScenarioResult measure(const QString &name, int warmupIterations,
                           int measuredIterations,
                           ProbeDelta expectedProbeDeltaPerFrame,
                           qsizetype expectedFinalKittyTextureSetCount,
                           quint64 expectedFinalKittyTextureBytes,
                           const std::function<bool()> &setupIteration,
                           const std::function<void()> &prepare,
                           RenderDocCapture *capture, QString *error)
    {
        const auto setup = [&] { return !setupIteration || setupIteration(); };
        for (int iteration = 0; iteration < warmupIterations; ++iteration) {
            if (!setup() || !renderFrame(prepare, nullptrTiming(), error)) {
                return {};
            }
        }

        const int frameCount = capture != nullptr ? 1 : measuredIterations;
        QVector<qint64> cpuUpdateSamples;
        QVector<qint64> cpuRecordSamples;
        QVector<qint64> cpuCompletionSamples;
        QVector<qint64> cpuTotalSamples;
        QVector<qint64> gpuSamples;
        cpuUpdateSamples.reserve(frameCount);
        cpuRecordSamples.reserve(frameCount);
        cpuCompletionSamples.reserve(frameCount);
        cpuTotalSamples.reserve(frameCount);
        gpuSamples.reserve(frameCount);
        ProbeDelta accumulatedDelta;
        qsizetype finalTextureSetCount = 0;
        quint64 finalTextureBytes = 0;
        qsizetype finalNativeTextNodeCount = 0;
        quint64 finalGlyphAtlasSerial = 0;
        QVector<quint64> finalRowContainerSerials;
        QVector<quint64> finalRowGlyphBatchSerials;
        QVector<quint64> finalRowNativeTextNodeSerials;
        qsizetype finalGlyphAtlasEntryCount = 0;
        quint64 finalGlyphAtlasBytes = 0;
        for (int iteration = 0; iteration < frameCount; ++iteration) {
            if (!setup()) return {};
            const TerminalPaneRenderProbeSnapshot before =
                terminalPaneRenderProbe(pane_);
            FrameTiming timing;
            std::optional<RenderDocCaptureScope> captureScope;
            if (capture != nullptr) {
                captureScope.emplace(*capture);
                if (!captureScope->started()) {
                    *error =
                        QStringLiteral("unable to start RenderDoc capture: %1")
                            .arg(capture->errorString());
                    return {};
                }
            }
            if (!renderFrame(prepare, &timing, error)) return {};
            if (captureScope.has_value() && !captureScope->finish()) {
                *error =
                    QStringLiteral("unable to finish RenderDoc capture: %1")
                        .arg(capture->errorString());
                return {};
            }
            const TerminalPaneRenderProbeSnapshot after =
                terminalPaneRenderProbe(pane_);
            accumulatedDelta += after - before;
            finalTextureSetCount = after.kittyGraphicsTextureCount;
            finalTextureBytes = after.kittyGraphicsTextureBytes;
            finalNativeTextNodeCount = after.nativeTextNodeCount;
            finalGlyphAtlasSerial = after.glyphAtlasSerial;
            finalRowContainerSerials = after.rowContainerSerials;
            finalRowGlyphBatchSerials = after.rowGlyphBatchSerials;
            finalRowNativeTextNodeSerials = after.rowNodeSerials;
            finalGlyphAtlasEntryCount = after.glyphAtlasEntryCount;
            finalGlyphAtlasBytes = after.glyphAtlasBytes;
            if (timing.cpuUpdateNanoseconds.has_value()) {
                cpuUpdateSamples.append(*timing.cpuUpdateNanoseconds);
            }
            if (timing.cpuRecordNanoseconds.has_value()) {
                cpuRecordSamples.append(*timing.cpuRecordNanoseconds);
            }
            if (timing.cpuCompletionNanoseconds.has_value()) {
                cpuCompletionSamples.append(*timing.cpuCompletionNanoseconds);
            }
            cpuTotalSamples.append(timing.cpuTotalNanoseconds);
            if (timing.gpuNanoseconds.has_value()) {
                gpuSamples.append(*timing.gpuNanoseconds);
            }
        }

        return {
            .name = name,
            .captureFrame = capture != nullptr,
            .cpuUpdateTiming = cpuUpdateSamples.size() == frameCount
                ? std::optional<TimingSummary>(
                      summarize(std::move(cpuUpdateSamples)))
                : std::nullopt,
            .cpuRecordTiming = cpuRecordSamples.size() == frameCount
                ? std::optional<TimingSummary>(
                      summarize(std::move(cpuRecordSamples)))
                : std::nullopt,
            .cpuCompletionTiming = cpuCompletionSamples.size() == frameCount
                ? std::optional<TimingSummary>(
                      summarize(std::move(cpuCompletionSamples)))
                : std::nullopt,
            .cpuTotalTiming = summarize(std::move(cpuTotalSamples)),
            .gpuTiming = gpuSamples.size() == frameCount
                ? std::optional<TimingSummary>(summarize(gpuSamples))
                : std::nullopt,
            .validGpuSamples = static_cast<int>(gpuSamples.size()),
            .probeDelta = accumulatedDelta,
            .expectedProbeDelta =
                expectedProbeDeltaPerFrame * static_cast<quint64>(frameCount),
            .finalKittyTextureSetCount = finalTextureSetCount,
            .expectedFinalKittyTextureSetCount =
                expectedFinalKittyTextureSetCount,
            .finalKittyTextureBytes = finalTextureBytes,
            .expectedFinalKittyTextureBytes = expectedFinalKittyTextureBytes,
            .finalNativeTextNodeCount = finalNativeTextNodeCount,
            .finalGlyphAtlasSerial = finalGlyphAtlasSerial,
            .finalRowContainerSerials = std::move(finalRowContainerSerials),
            .finalRowGlyphBatchSerials = std::move(finalRowGlyphBatchSerials),
            .finalRowNativeTextNodeSerials =
                std::move(finalRowNativeTextNodeSerials),
            .finalGlyphAtlasEntryCount = finalGlyphAtlasEntryCount,
            .finalGlyphAtlasBytes = finalGlyphAtlasBytes,
            .measuredFrames = frameCount,
        };
    }

    FrameTiming *nullptrTiming()
    {
        warmupTiming_ = {};
        return &warmupTiming_;
    }

    GridSize grid_;
    GraphicsApi graphicsApi_ = GraphicsApi::Software;
#if GHOSTTY_QT_PANE_BENCH_HAS_VULKAN
    std::unique_ptr<QVulkanInstance> vulkanInstance_;
#endif
    std::unique_ptr<QQuickRenderControl> renderControl_;
    std::unique_ptr<QQuickWindow> window_;
    std::unique_ptr<QRhiTexture> colorBuffer_;
    std::unique_ptr<QRhiRenderBuffer> depthStencil_;
    std::unique_ptr<QRhiRenderPassDescriptor> renderPassDescriptor_;
    std::unique_ptr<QRhiTextureRenderTarget> renderTarget_;
    TerminalPane *pane_ = nullptr;
    TerminalController *controller_ = nullptr;
    QSize logicalSize_;
    QSize framebufferSize_;
    std::optional<QRhiDriverInfo> driverInfo_;
    QString fontError_;
    quint64 revision_ = 0;
    quint64 searchGeneration_ = 0;
    quint64 kittyGeneration_ = 0;
    int cursorRow_ = 0;
    FrameTiming warmupTiming_;
};

std::optional<int> positiveInteger(const QString &value)
{
    bool ok = false;
    const int result = value.toInt(&ok);
    if (!ok || result <= 0) return std::nullopt;
    return result;
}

std::optional<GraphicsApi> parseGraphicsApi(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1StringView("software")) {
        return GraphicsApi::Software;
    }
    if (normalized == QLatin1StringView("opengl")) {
        return GraphicsApi::OpenGl;
    }
    if (normalized == QLatin1StringView("vulkan")) {
#if GHOSTTY_QT_PANE_BENCH_HAS_VULKAN
        return GraphicsApi::Vulkan;
#else
        return std::nullopt;
#endif
    }
    return std::nullopt;
}

using ScenarioFunction = ScenarioResult (RendererBenchmark::*)(
    int, int, RenderDocCapture *, QString *);

const QVector<std::pair<QString, ScenarioFunction>> &scenarioFunctions()
{
    static const QVector<std::pair<QString, ScenarioFunction>> functions{
        {QStringLiteral("metadata"), &RendererBenchmark::metadata},
        {QStringLiteral("one-dirty-row"), &RendererBenchmark::oneDirtyRow},
        {QStringLiteral("text-ascii-dirty-row"),
         &RendererBenchmark::textAsciiDirtyRow},
        {QStringLiteral("text-ligature-dirty-row"),
         &RendererBenchmark::textLigatureDirtyRow},
        {QStringLiteral("text-ascii-full-frame"),
         &RendererBenchmark::textAsciiFullFrame},
        {QStringLiteral("text-ligature-full-frame"),
         &RendererBenchmark::textLigatureFullFrame},
        {QStringLiteral("text-atlas-retained-rebuild"),
         &RendererBenchmark::textAtlasRetainedRebuild},
        {QStringLiteral("text-explicit-color-global-noop"),
         &RendererBenchmark::textExplicitColorGlobalNoop},
        {QStringLiteral("text-selective-color-change"),
         &RendererBenchmark::textSelectiveColorChange},
        {QStringLiteral("cursor-only"), &RendererBenchmark::cursorOnly},
        {QStringLiteral("full-invalidation"),
         &RendererBenchmark::fullInvalidation},
        {QStringLiteral("search-update"), &RendererBenchmark::searchUpdate},
        {QStringLiteral("search-selection-update"),
         &RendererBenchmark::searchSelectionUpdate},
        {QStringLiteral("search-clear"), &RendererBenchmark::searchClear},
        {QStringLiteral("kitty-first-upload"),
         &RendererBenchmark::kittyFirstUpload},
        {QStringLiteral("kitty-translucent-first-upload"),
         &RendererBenchmark::kittyTranslucentFirstUpload},
        {QStringLiteral("kitty-retained-redraw"),
         &RendererBenchmark::kittyRetainedRedraw},
        {QStringLiteral("kitty-translucent-retained-redraw"),
         &RendererBenchmark::kittyTranslucentRetainedRedraw},
        {QStringLiteral("kitty-movement"), &RendererBenchmark::kittyMovement},
        {QStringLiteral("kitty-replacement"),
         &RendererBenchmark::kittyReplacement},
        {QStringLiteral("kitty-translucent-replacement"),
         &RendererBenchmark::kittyTranslucentReplacement},
        {QStringLiteral("kitty-eviction"), &RendererBenchmark::kittyEviction},
    };
    return functions;
}

void printSummary(QTextStream &output, QStringView prefix,
                  const std::optional<TimingSummary> &summary)
{
    output << ' ' << prefix << "_median_us=";
    if (!summary.has_value()) {
        output << "unavailable";
        return;
    }
    output << QString::number(summary->medianMicroseconds, 'f', 1) << ' '
           << prefix << "_p90_us="
           << QString::number(summary->percentile90Microseconds, 'f', 1) << ' '
           << prefix
           << "_mean_us=" << QString::number(summary->meanMicroseconds, 'f', 1)
           << ' ' << prefix << "_min_us="
           << QString::number(summary->minimumMicroseconds, 'f', 1);
}

bool printResult(GridSize grid, const ScenarioResult &result,
                 QTextStream &output)
{
    const QStringView rowContainerTopologyStatus =
        !result.expectedFinalRowContainerSerials.has_value() ? u"unvalidated"
        : result.finalRowContainerSerials
            == *result.expectedFinalRowContainerSerials
        ? u"true"
        : u"false";
    const QStringView rowGlyphBatchTopologyStatus =
        !result.expectedFinalRowGlyphBatchSerials.has_value() ? u"unvalidated"
        : result.finalRowGlyphBatchSerials
            == *result.expectedFinalRowGlyphBatchSerials
        ? u"true"
        : u"false";
    const QStringView rowNativeTextTopologyStatus =
        !result.expectedFinalRowNativeTextNodeSerials.has_value()
        ? u"unvalidated"
        : result.finalRowNativeTextNodeSerials
            == *result.expectedFinalRowNativeTextNodeSerials
        ? u"true"
        : u"false";

    output << grid.columns << 'x' << grid.rows << ' ' << result.name;
    if (result.captureFrame) {
        output << " capture_frame=true";
    } else {
        printSummary(output, u"cpu_update", result.cpuUpdateTiming);
        printSummary(output, u"cpu_record", result.cpuRecordTiming);
        printSummary(output, u"cpu_completion", result.cpuCompletionTiming);
        printSummary(output, u"cpu_total", result.cpuTotalTiming);
        printSummary(output, u"gpu", result.gpuTiming);
        output << " gpu_valid_samples=" << result.validGpuSamples << '/'
               << result.measuredFrames;
    }
    output << " measured_frames=" << result.measuredFrames
           << " paints=" << result.probeDelta.paintSerial
           << " solid_cell_visits=" << result.probeDelta.solidCellVisits
           << " solid_cell_visits_per_frame="
           << QString::number(
                  static_cast<double>(result.probeDelta.solidCellVisits)
                      / static_cast<double>(result.measuredFrames),
                  'f', 1)
           << " expected_solid_cell_visits="
           << result.expectedProbeDelta.solidCellVisits
           << " text_row_builds=" << result.probeDelta.textRowBuilds
           << " expected_text_row_builds="
           << result.expectedProbeDelta.textRowBuilds
           << " text_layouts=" << result.probeDelta.textLayouts
           << " text_layouts_per_frame="
           << QString::number(static_cast<double>(result.probeDelta.textLayouts)
                                  / static_cast<double>(result.measuredFrames),
                              'f', 1)
           << " text_fallback_cells=" << result.probeDelta.textFallbackCells
           << " text_fallback_cells_per_frame="
           << QString::number(
                  static_cast<double>(result.probeDelta.textFallbackCells)
                      / static_cast<double>(result.measuredFrames),
                  'f', 1)
           << " native_text_submissions="
           << result.probeDelta.nativeTextSubmissions
           << " native_text_cells=" << result.probeDelta.nativeTextCells
           << " native_text_nodes_final=" << result.finalNativeTextNodeCount
           << " batched_glyphs=" << result.probeDelta.batchedGlyphs
           << " glyph_batch_geometry_writes="
           << result.probeDelta.glyphBatchGeometryWrites
           << " glyph_batch_node_creations="
           << result.probeDelta.glyphBatchNodeCreations
           << " glyph_batch_allocations="
           << result.probeDelta.glyphBatchAllocations
           << " row_container_topology_stable=" << rowContainerTopologyStatus
           << " row_glyph_batch_topology_stable=" << rowGlyphBatchTopologyStatus
           << " row_native_text_topology_stable=" << rowNativeTextTopologyStatus
           << " glyph_atlas_uploads=" << result.probeDelta.glyphAtlasUploads
           << " glyph_atlas_serial_final=" << result.finalGlyphAtlasSerial
           << " glyph_atlas_entry_count_delta="
           << result.probeDelta.glyphAtlasEntryCount
           << " glyph_atlas_entry_count_final="
           << result.finalGlyphAtlasEntryCount
           << " glyph_atlas_bytes_delta=" << result.probeDelta.glyphAtlasBytes
           << " glyph_atlas_bytes_final=" << result.finalGlyphAtlasBytes
           << " kitty_texture_uploads=" << result.probeDelta.kittyTextureUploads
           << " kitty_node_creations=" << result.probeDelta.kittyNodeCreations
           << " kitty_node_deletions=" << result.probeDelta.kittyNodeDeletions
           << " kitty_geometry_writes=" << result.probeDelta.kittyGeometryWrites
           << " kitty_material_assignments="
           << result.probeDelta.kittyMaterialAssignments
           << " kitty_texture_set_evictions="
           << result.probeDelta.kittyTextureSetEvictions
           << " kitty_texture_set_count_delta="
           << result.probeDelta.kittyTextureSetCount
           << " kitty_texture_set_count_final="
           << result.finalKittyTextureSetCount
           << " kitty_texture_bytes_final=" << result.finalKittyTextureBytes
           << '\n';

    bool valid = true;
    const auto requireEqual = [&](auto actual, auto expected,
                                  QStringView field) {
        if (actual == expected) return;
        output << "unexpected " << field << " for " << result.name
               << ": actual=" << actual << " expected=" << expected << '\n';
        valid = false;
    };
    requireEqual(result.probeDelta.paintSerial,
                 result.expectedProbeDelta.paintSerial, u"paint count");
    requireEqual(result.probeDelta.solidCellVisits,
                 result.expectedProbeDelta.solidCellVisits,
                 u"solid-cell visit count");
    requireEqual(result.probeDelta.textRowBuilds,
                 result.expectedProbeDelta.textRowBuilds,
                 u"text-row build count");
    requireEqual(result.probeDelta.textLayouts,
                 result.expectedProbeDelta.textLayouts, u"text layout count");
    requireEqual(result.probeDelta.textFallbackCells,
                 result.expectedProbeDelta.textFallbackCells,
                 u"text fallback-cell count");
    requireEqual(result.probeDelta.nativeTextSubmissions,
                 result.expectedProbeDelta.nativeTextSubmissions,
                 u"native text submission count");
    requireEqual(result.probeDelta.nativeTextCells,
                 result.expectedProbeDelta.nativeTextCells,
                 u"native text cell count");
    requireEqual(result.probeDelta.batchedGlyphs,
                 result.expectedProbeDelta.batchedGlyphs,
                 u"batched glyph count");
    requireEqual(result.probeDelta.glyphBatchGeometryWrites,
                 result.expectedProbeDelta.glyphBatchGeometryWrites,
                 u"glyph-batch geometry write count");
    requireEqual(result.probeDelta.glyphBatchNodeCreations,
                 result.expectedProbeDelta.glyphBatchNodeCreations,
                 u"glyph-batch node creation count");
    requireEqual(result.probeDelta.glyphBatchAllocations,
                 result.expectedProbeDelta.glyphBatchAllocations,
                 u"glyph-batch allocation count");
    requireEqual(result.probeDelta.glyphAtlasUploads,
                 result.expectedProbeDelta.glyphAtlasUploads,
                 u"glyph-atlas upload count");
    requireEqual(result.probeDelta.glyphAtlasEntryCount,
                 result.expectedProbeDelta.glyphAtlasEntryCount,
                 u"glyph-atlas entry-count delta");
    requireEqual(result.probeDelta.glyphAtlasBytes,
                 result.expectedProbeDelta.glyphAtlasBytes,
                 u"glyph-atlas byte delta");
    if (result.expectedProbeDelta.batchedGlyphs > 0
        && (result.finalGlyphAtlasSerial == 0
            || result.finalGlyphAtlasEntryCount <= 0
            || result.finalGlyphAtlasBytes == 0)) {
        output << "missing resident glyph atlas for " << result.name
               << ": serial=" << result.finalGlyphAtlasSerial
               << " entries=" << result.finalGlyphAtlasEntryCount
               << " bytes=" << result.finalGlyphAtlasBytes << '\n';
        valid = false;
    }
    if (result.expectedFinalNativeTextNodeCount.has_value()) {
        requireEqual(result.finalNativeTextNodeCount,
                     *result.expectedFinalNativeTextNodeCount,
                     u"final native text-node count");
    }
    if (result.expectedFinalGlyphAtlasSerial.has_value()) {
        requireEqual(result.finalGlyphAtlasSerial,
                     *result.expectedFinalGlyphAtlasSerial,
                     u"final glyph-atlas serial");
    }
    const auto requireStableTopology =
        [&](const QVector<quint64> &actual,
            const std::optional<QVector<quint64>> &expected,
            QStringView field) {
            if (!expected.has_value() || actual == *expected) return;
            qsizetype mismatch = 0;
            const qsizetype sharedSize =
                std::min(actual.size(), expected->size());
            while (mismatch < sharedSize
                   && actual.at(mismatch) == expected->at(mismatch)) {
                ++mismatch;
            }
            output << "unstable " << field << " for " << result.name
                   << ": actual_rows=" << actual.size()
                   << " expected_rows=" << expected->size();
            if (mismatch < sharedSize) {
                output << " first_mismatch_row=" << mismatch
                       << " actual_serial=" << actual.at(mismatch)
                       << " expected_serial=" << expected->at(mismatch);
            }
            output << '\n';
            valid = false;
        };
    requireStableTopology(result.finalRowContainerSerials,
                          result.expectedFinalRowContainerSerials,
                          u"row-container topology");
    requireStableTopology(result.finalRowGlyphBatchSerials,
                          result.expectedFinalRowGlyphBatchSerials,
                          u"row glyph-batch topology");
    requireStableTopology(result.finalRowNativeTextNodeSerials,
                          result.expectedFinalRowNativeTextNodeSerials,
                          u"row native-text topology");
    requireEqual(result.probeDelta.kittyTextureUploads,
                 result.expectedProbeDelta.kittyTextureUploads,
                 u"Kitty texture upload count");
    requireEqual(result.probeDelta.kittyNodeCreations,
                 result.expectedProbeDelta.kittyNodeCreations,
                 u"Kitty node creation count");
    requireEqual(result.probeDelta.kittyNodeDeletions,
                 result.expectedProbeDelta.kittyNodeDeletions,
                 u"Kitty node deletion count");
    requireEqual(result.probeDelta.kittyGeometryWrites,
                 result.expectedProbeDelta.kittyGeometryWrites,
                 u"Kitty geometry write count");
    requireEqual(result.probeDelta.kittyMaterialAssignments,
                 result.expectedProbeDelta.kittyMaterialAssignments,
                 u"Kitty material assignment count");
    requireEqual(result.probeDelta.kittyTextureSetEvictions,
                 result.expectedProbeDelta.kittyTextureSetEvictions,
                 u"Kitty texture-set eviction count");
    requireEqual(result.probeDelta.kittyTextureSetCount,
                 result.expectedProbeDelta.kittyTextureSetCount,
                 u"Kitty texture-set count delta");
    requireEqual(result.finalKittyTextureSetCount,
                 result.expectedFinalKittyTextureSetCount,
                 u"final Kitty texture-set count");
    requireEqual(result.finalKittyTextureBytes,
                 result.expectedFinalKittyTextureBytes,
                 u"final Kitty logical texture bytes");
    return valid;
}

int runGrid(GridSize grid, GraphicsApi graphicsApi, int warmupIterations,
            int measuredIterations, const QString &selectedScenario,
            RenderDocCapture *capture, QTextStream &output)
{
    QString error;
    RendererBenchmark benchmark(grid, graphicsApi, capture != nullptr);
    const InitializationResult initialized = benchmark.initialize(&error);
    if (initialized != InitializationResult::Success) {
        output << (initialized == InitializationResult::BackendUnavailable
                       ? "backend unavailable for "
                       : "failed to initialize ")
               << grid.columns << 'x' << grid.rows << ": " << error
               << " dpr=" << benchmark.devicePixelRatio()
               << " logical=" << benchmark.logicalSize().width() << 'x'
               << benchmark.logicalSize().height()
               << " framebuffer=" << benchmark.framebufferSize().width() << 'x'
               << benchmark.framebufferSize().height() << '\n';
        return initialized == InitializationResult::BackendUnavailable ? 77 : 1;
    }
    const QSize framebuffer = benchmark.framebufferSize();
    const QSize logical = benchmark.logicalSize();
    output << "grid=" << grid.columns << 'x' << grid.rows
           << " rhi_backend=" << outputToken(benchmark.rhiBackendName())
           << " dpr=" << benchmark.devicePixelRatio()
           << " logical=" << logical.width() << 'x' << logical.height()
           << " framebuffer=" << framebuffer.width() << 'x'
           << framebuffer.height();
    if (benchmark.driverInfo().has_value()) {
        const QRhiDriverInfo &driver = *benchmark.driverInfo();
        output << " rhi_device_name="
               << outputToken(QString::fromUtf8(driver.deviceName))
               << " rhi_device_type=" << deviceTypeName(driver.deviceType)
               << " rhi_vendor_id=0x" << QString::number(driver.vendorId, 16)
               << " rhi_device_id=0x" << QString::number(driver.deviceId, 16);
    }
    output << '\n';

    for (const auto &[name, function] : scenarioFunctions()) {
        if (!selectedScenario.isEmpty() && name != selectedScenario) continue;
        const ScenarioResult result = (benchmark.*function)(
            warmupIterations, measuredIterations, capture, &error);
        if (result.name.isEmpty()) {
            output << "failed to render " << grid.columns << 'x' << grid.rows
                   << ' ' << name << ": " << error << '\n';
            return 1;
        }
        if (!printResult(grid, result, output)) return 2;
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    if (qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")) {
        qputenv("QT_SCALE_FACTOR", QByteArrayLiteral("1"));
    }
    QStandardPaths::setTestModeEnabled(true);

    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-pane-renderer"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Warmed ghostty-qt incremental-render benchmark"));
    parser.addHelpOption();
    const QCommandLineOption graphicsApiOption(
        QStringLiteral("graphics-api"),
        QStringLiteral("Scene-graph backend: software, opengl, or vulkan."),
        QStringLiteral("api"), QStringLiteral("software"));
    const QCommandLineOption warmupOption(
        QStringList{QStringLiteral("w"), QStringLiteral("warmup")},
        QStringLiteral("Warmup frames per scenario."), QStringLiteral("count"),
        QStringLiteral("8"));
    const QCommandLineOption iterationsOption(
        QStringList{QStringLiteral("n"), QStringLiteral("iterations")},
        QStringLiteral("Measured frames per scenario."),
        QStringLiteral("count"), QStringLiteral("30"));
    const QCommandLineOption renderDocCaptureOption(
        QStringLiteral("renderdoc-capture"),
        QStringLiteral("Capture one named scenario after warmup."),
        QStringLiteral("scenario"));
    const QCommandLineOption scenarioOption(
        QStringLiteral("scenario"),
        QStringLiteral("Run one named scenario without a RenderDoc capture."),
        QStringLiteral("scenario"));
    const QCommandLineOption renderDocCapturePathOption(
        QStringLiteral("renderdoc-capture-path"),
        QStringLiteral("UTF-8 RenderDoc capture filename template."),
        QStringLiteral("path"));
    const QCommandLineOption listScenariosOption(
        QStringLiteral("list-scenarios"),
        QStringLiteral("List authoritative scenario names and exit."));
    parser.addOption(graphicsApiOption);
    parser.addOption(warmupOption);
    parser.addOption(iterationsOption);
    parser.addOption(renderDocCaptureOption);
    parser.addOption(scenarioOption);
    parser.addOption(renderDocCapturePathOption);
    parser.addOption(listScenariosOption);
    parser.process(application);

    if (parser.isSet(listScenariosOption)) {
        QTextStream output(stdout);
        for (const auto &[name, function] : scenarioFunctions()) {
            Q_UNUSED(function);
            output << name << '\n';
        }
        return 0;
    }

    const std::optional<int> warmup =
        positiveInteger(parser.value(warmupOption));
    const std::optional<int> iterations =
        positiveInteger(parser.value(iterationsOption));
    if (!warmup.has_value() || !iterations.has_value()) {
        QTextStream(stderr)
            << "--warmup and --iterations must be positive integers\n";
        return 2;
    }
    const std::optional<GraphicsApi> graphicsApi =
        parseGraphicsApi(parser.value(graphicsApiOption));
    if (!graphicsApi.has_value()) {
#if !GHOSTTY_QT_PANE_BENCH_HAS_VULKAN
        if (parser.value(graphicsApiOption)
                .trimmed()
                .compare(QStringLiteral("vulkan"), Qt::CaseInsensitive)
            == 0) {
            QTextStream(stderr) << "vulkan is unavailable in this Qt build\n";
            return 77;
        }
#endif
        QTextStream(stderr)
            << "graphics-api must be software, opengl, or a supported vulkan "
               "build\n";
        return 2;
    }
    switch (*graphicsApi) {
    case GraphicsApi::Software:
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        break;
    case GraphicsApi::OpenGl:
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
        break;
    case GraphicsApi::Vulkan:
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        break;
    }

    const QString captureScenario =
        parser.value(renderDocCaptureOption).trimmed();
    const QString requestedScenario = parser.value(scenarioOption).trimmed();
    if (!captureScenario.isEmpty() && !requestedScenario.isEmpty()) {
        QTextStream(stderr)
            << "--scenario and --renderdoc-capture are mutually exclusive\n";
        return 2;
    }
    const QString selectedScenario =
        captureScenario.isEmpty() ? requestedScenario : captureScenario;
    if (!selectedScenario.isEmpty()) {
        const auto found = std::ranges::find_if(
            scenarioFunctions(), [&selectedScenario](const auto &entry) {
                return entry.first == selectedScenario;
            });
        if (found == scenarioFunctions().cend()) {
            QTextStream(stderr)
                << "unknown scenario " << selectedScenario << '\n';
            return 2;
        }
    }
    if (!captureScenario.isEmpty() && *graphicsApi == GraphicsApi::Software) {
        QTextStream(stderr) << "RenderDoc capture requires opengl or vulkan\n";
        return 2;
    }
    if (captureScenario.isEmpty() && parser.isSet(renderDocCapturePathOption)) {
        QTextStream(stderr)
            << "renderdoc-capture-path requires renderdoc-capture\n";
        return 2;
    }

    std::unique_ptr<RenderDocCapture> capture;
    if (!captureScenario.isEmpty()) {
        capture = std::make_unique<RenderDocCapture>();
        if (!capture->isAvailable()) {
            QTextStream(stderr) << "unable to initialize RenderDoc capture: "
                                << capture->errorString() << '\n';
            return 4;
        }
        if (parser.isSet(renderDocCapturePathOption)
            && !capture->setCapturePathTemplate(
                parser.value(renderDocCapturePathOption).toUtf8())) {
            QTextStream(stderr) << "unable to configure RenderDoc capture: "
                                << capture->errorString() << '\n';
            return 4;
        }
    }

    QTextStream output(stdout);
    output << "qt_version=" << outputToken(QString::fromLatin1(qVersion()))
           << " benchmark_contract=1"
           << " backend=" << graphicsApiName(*graphicsApi)
           << " platform=" << QGuiApplication::platformName()
           << " presentation=offscreen"
           << " warmup=" << *warmup
           << " iterations=" << (capture != nullptr ? 1 : *iterations)
           << " kitty_placements=" << kittyPlacementCount;
    if (capture != nullptr) {
        output << " renderdoc_capture=" << captureScenario
               << " renderdoc_api=" << capture->apiVersion()
               << " requested_iterations=" << *iterations;
    }
    output << '\n';

    const QVector<GridSize> grids = capture != nullptr
        ? QVector<GridSize>{{.columns = 120, .rows = 40}}
        : QVector<GridSize>{
              {.columns = 120, .rows = 40},
              {.columns = 240, .rows = 80},
          };
    for (const GridSize grid : grids) {
        const int result = runGrid(grid, *graphicsApi, *warmup, *iterations,
                                   selectedScenario, capture.get(), output);
        if (result != 0) return result;
    }
    if (capture != nullptr) {
        output << "renderdoc_capture_complete=" << captureScenario << '\n';
    }
    return 0;
}
