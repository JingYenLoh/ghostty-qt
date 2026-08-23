#include "terminal/adapter/ghostty_vt_adapter.h"
#include "terminal/adapter/terminal_kitty_image_materialization.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QTextStream>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>

namespace {

constexpr qsizetype kittyPayloadBytes = 4'096;
constexpr quint64 maximumDecodedFrameBytes = 400ULL * 1'024ULL * 1'024ULL;

struct FrameCommands {
    QVector<QByteArray> packets;
    quint64 rawBytes = 0;
    quint64 wireBytes = 0;
    int kittyFormat = 24;
    bool fullyOpaque = true;
};

struct TimingSummary {
    double minimumMicroseconds = 0.0;
    double medianMicroseconds = 0.0;
    double percentile90Microseconds = 0.0;
    double meanMicroseconds = 0.0;
    qint64 totalNanoseconds = 0;
};

enum class SourcePattern {
    Rgb24,
    Rgba32Varied,
    Rgba32Opaque,
    Rgba32FirstTranslucent,
    Rgba32LastTranslucent,
    Gray8,
    GrayAlpha,
};

enum class Workload {
    Protocol,
    Materialization,
};

struct SourceFrame {
    QByteArray pixels;
    bool fullyOpaque = true;
};

std::optional<SourcePattern> sourcePattern(const QString &name)
{
    if (name == QStringLiteral("rgb24")) return SourcePattern::Rgb24;
    if (name == QStringLiteral("rgba32")) return SourcePattern::Rgba32Varied;
    if (name == QStringLiteral("rgba32-opaque")) {
        return SourcePattern::Rgba32Opaque;
    }
    if (name == QStringLiteral("rgba32-first-translucent")) {
        return SourcePattern::Rgba32FirstTranslucent;
    }
    if (name == QStringLiteral("rgba32-last-translucent")) {
        return SourcePattern::Rgba32LastTranslucent;
    }
    if (name == QStringLiteral("gray8")) return SourcePattern::Gray8;
    if (name == QStringLiteral("gray-alpha")) return SourcePattern::GrayAlpha;
    return std::nullopt;
}

std::optional<Workload> workload(const QString &name)
{
    if (name == QStringLiteral("protocol")) return Workload::Protocol;
    if (name == QStringLiteral("materialization")) {
        return Workload::Materialization;
    }
    return std::nullopt;
}

quint64 bytesPerPixel(SourcePattern pattern)
{
    switch (pattern) {
    case SourcePattern::Rgb24: return 3;
    case SourcePattern::Rgba32Varied:
    case SourcePattern::Rgba32Opaque:
    case SourcePattern::Rgba32FirstTranslucent:
    case SourcePattern::Rgba32LastTranslucent: return 4;
    case SourcePattern::Gray8: return 1;
    case SourcePattern::GrayAlpha: return 2;
    }
    return 0;
}

bool expectedFullyOpaque(SourcePattern pattern)
{
    return pattern == SourcePattern::Rgb24
        || pattern == SourcePattern::Rgba32Opaque
        || pattern == SourcePattern::Gray8;
}

std::optional<int> protocolFormat(SourcePattern pattern)
{
    switch (pattern) {
    case SourcePattern::Rgb24: return 24;
    case SourcePattern::Rgba32Varied:
    case SourcePattern::Rgba32Opaque:
    case SourcePattern::Rgba32FirstTranslucent:
    case SourcePattern::Rgba32LastTranslucent: return 32;
    case SourcePattern::Gray8:
    case SourcePattern::GrayAlpha: return std::nullopt;
    }
    return std::nullopt;
}

TerminalKittyPixelFormat materializationFormat(SourcePattern pattern)
{
    switch (pattern) {
    case SourcePattern::Rgb24: return TerminalKittyPixelFormat::Rgb;
    case SourcePattern::Rgba32Varied:
    case SourcePattern::Rgba32Opaque:
    case SourcePattern::Rgba32FirstTranslucent:
    case SourcePattern::Rgba32LastTranslucent:
        return TerminalKittyPixelFormat::Rgba;
    case SourcePattern::Gray8: return TerminalKittyPixelFormat::Gray;
    case SourcePattern::GrayAlpha: return TerminalKittyPixelFormat::GrayAlpha;
    }
    return TerminalKittyPixelFormat::Rgba;
}

std::optional<int> integerOption(const QString &value, bool allowZero = false)
{
    bool ok = false;
    const int result = value.toInt(&ok);
    if (!ok || result < (allowZero ? 0 : 1)) return std::nullopt;
    return result;
}

std::optional<quint64> processStatusKiB(const QByteArray &key)
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) return std::nullopt;

    // procfs files report a size of zero, so QFile::atEnd() is true before
    // the first read even though the generated contents remain readable.
    const QList<QByteArray> lines = status.readAll().split('\n');
    for (const QByteArray &line : lines) {
        if (!line.startsWith(key)) continue;
        QByteArray value = line.sliced(key.size()).trimmed();
        const qsizetype separator = value.indexOf(' ');
        if (separator >= 0) value.truncate(separator);
        bool ok = false;
        const quint64 result = value.toULongLong(&ok);
        return ok ? std::optional<quint64>(result) : std::nullopt;
    }
    return std::nullopt;
}

std::optional<SourceFrame>
makeSourceFrame(int width, int height, SourcePattern pattern, QString *error)
{
    const quint64 pixelStride = bytesPerPixel(pattern);
    const quint64 rawBytes = static_cast<quint64>(width)
        * static_cast<quint64>(height) * pixelStride;
    if (rawBytes > maximumDecodedFrameBytes
        || rawBytes
            > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        *error =
            QStringLiteral("decoded frame exceeds the Kitty 400 MiB limit");
        return std::nullopt;
    }

    QByteArray raw(static_cast<qsizetype>(rawBytes), Qt::Uninitialized);
    const quint64 pixelCount =
        static_cast<quint64>(width) * static_cast<quint64>(height);
    for (quint64 pixel = 0; pixel < pixelCount; ++pixel) {
        const qsizetype offset = static_cast<qsizetype>(pixel * pixelStride);
        switch (pattern) {
        case SourcePattern::Rgb24:
        case SourcePattern::Rgba32Varied:
        case SourcePattern::Rgba32Opaque:
        case SourcePattern::Rgba32FirstTranslucent:
        case SourcePattern::Rgba32LastTranslucent:
            raw[offset] = static_cast<char>((pixel * 17ULL + 23ULL) % 251ULL);
            raw[offset + 1] =
                static_cast<char>((pixel * 31ULL + 47ULL) % 251ULL);
            raw[offset + 2] =
                static_cast<char>((pixel * 43ULL + 71ULL) % 251ULL);
            break;
        case SourcePattern::Gray8:
        case SourcePattern::GrayAlpha:
            raw[offset] = static_cast<char>((pixel * 17ULL + 23ULL) % 251ULL);
            break;
        }

        if (pixelStride == 4) {
            uchar alpha = 255;
            switch (pattern) {
            case SourcePattern::Rgb24: break;
            case SourcePattern::Rgba32Varied:
                // Preserve the original benchmark workload as `rgba32`.
                alpha = static_cast<uchar>((pixel * 29ULL + 31ULL) % 251ULL);
                break;
            case SourcePattern::Rgba32Opaque: break;
            case SourcePattern::Rgba32FirstTranslucent:
                if (pixel == 0) alpha = 127;
                break;
            case SourcePattern::Rgba32LastTranslucent:
                if (pixel + 1 == pixelCount) alpha = 127;
                break;
            case SourcePattern::Gray8:
            case SourcePattern::GrayAlpha: break;
            }
            raw[offset + 3] = static_cast<char>(alpha);
        } else if (pattern == SourcePattern::GrayAlpha) {
            raw[offset + 1] =
                static_cast<char>((pixel * 29ULL + 31ULL) % 251ULL);
        }
    }
    return SourceFrame{
        .pixels = std::move(raw),
        .fullyOpaque = expectedFullyOpaque(pattern),
    };
}

FrameCommands makeExplicitReplacementFrame(const SourceFrame &source,
                                           int kittyFormat, int width,
                                           int height)
{
    const QByteArray encoded = source.pixels.toBase64();

    FrameCommands result;
    result.rawBytes = static_cast<quint64>(source.pixels.size());
    result.kittyFormat = kittyFormat;
    result.fullyOpaque = source.fullyOpaque;
    result.packets.reserve(static_cast<qsizetype>(
        std::ceil(static_cast<double>(encoded.size())
                  / static_cast<double>(kittyPayloadBytes))));

    qsizetype offset = 0;
    bool first = true;
    while (offset < encoded.size()) {
        const qsizetype payloadSize =
            std::min(kittyPayloadBytes, encoded.size() - offset);
        const bool more = offset + payloadSize < encoded.size();
        QByteArray packet;
        if (first) {
            packet = QByteArrayLiteral("\033[H\033_Ga=T,i=1,p=1,f=")
                + QByteArray::number(result.kittyFormat)
                + QByteArrayLiteral(",s=") + QByteArray::number(width)
                + QByteArrayLiteral(",v=") + QByteArray::number(height)
                + QByteArrayLiteral(",C=1,q=2,m=")
                + (more ? QByteArrayLiteral("1;") : QByteArrayLiteral("0;"));
        } else {
            packet = QByteArrayLiteral("\033_Gm=")
                + (more ? QByteArrayLiteral("1;") : QByteArrayLiteral("0;"));
        }
        packet += QByteArrayView(encoded).sliced(offset, payloadSize);
        packet += QByteArrayLiteral("\033\\");
        result.wireBytes += static_cast<quint64>(packet.size());
        result.packets.append(std::move(packet));
        offset += payloadSize;
        first = false;
    }
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

bool importFrame(GhosttyVtAdapter *adapter, const FrameCommands &commands,
                 int width, int height, TerminalFrame *frame, QString *error)
{
    for (const QByteArray &packet : commands.packets) {
        adapter->writeVt(packet);
    }

    GhosttyVtAdapter::RenderSnapshot snapshot;
    if (adapter->renderFrame(&snapshot)
        != GhosttyVtAdapter::RenderResult::Ready) {
        *error = QStringLiteral("libghostty did not produce a ready frame");
        return false;
    }
    if (!snapshot.update.kittyGraphicsChanged) {
        *error = QStringLiteral("completed Kitty frame was not published");
        return false;
    }
    if (!applyTerminalUpdate(*frame, snapshot.update)) {
        *error = QStringLiteral("published terminal update was invalid");
        return false;
    }
    if (frame->kittyGraphics == nullptr
        || frame->kittyGraphics->placements.size() != 1) {
        *error =
            QStringLiteral("replacement frames did not retain one placement");
        return false;
    }

    const TerminalKittyGraphicsPlacement &placement =
        frame->kittyGraphics->placements.constFirst();
    const quint64 expectedPackedBytes =
        static_cast<quint64>(width) * static_cast<quint64>(height) * 4ULL;
    if (placement.image == nullptr
        || placement.image->fullyOpaque != commands.fullyOpaque
        || placement.image->straightRgba.format() != QImage::Format_RGBA8888
        || placement.image->straightRgba.size() != QSize(width, height)
        || static_cast<quint64>(placement.image->straightRgba.sizeInBytes())
            != expectedPackedBytes) {
        *error = QStringLiteral(
            "frame opacity or packed RGBA storage did not match the request");
        return false;
    }
    return true;
}

void printOptionalKiB(QTextStream &output, const std::optional<quint64> &value)
{
    if (value.has_value()) {
        output << *value;
    } else {
        output << "unavailable";
    }
}

bool validMaterialization(
    const std::optional<TerminalKittyImageMaterialization> &materialized,
    const SourceFrame &source, QSize size)
{
    const quint64 expectedPackedBytes = static_cast<quint64>(size.width())
        * static_cast<quint64>(size.height()) * 4ULL;
    return materialized.has_value()
        && materialized->fullyOpaque == source.fullyOpaque
        && materialized->straightRgba.format() == QImage::Format_RGBA8888
        && materialized->straightRgba.size() == size
        && static_cast<quint64>(materialized->straightRgba.sizeInBytes())
        == expectedPackedBytes;
}

int runMaterializationBenchmark(const SourceFrame &source,
                                SourcePattern pattern,
                                const QString &pixelFormat, int width,
                                int height, int warmup, int iterations,
                                quint64 totalFrames)
{
    const QSize size(width, height);
    const std::span<const std::uint8_t> pixels(
        reinterpret_cast<const std::uint8_t *>(source.pixels.constData()),
        static_cast<std::size_t>(source.pixels.size()));
    const TerminalKittyPixelFormat format = materializationFormat(pattern);
    std::optional<TerminalKittyImageMaterialization> latest;

    const std::optional<quint64> rssBefore =
        processStatusKiB(QByteArrayLiteral("VmRSS:"));
    for (int iteration = 0; iteration < warmup; ++iteration) {
        auto materialized = terminalMaterializeKittyImage(size, format, pixels);
        if (!validMaterialization(materialized, source, size)) {
            QTextStream(stderr) << "warmup materialization " << iteration
                                << ": packed image did not match the source\n";
            return 1;
        }
        latest = std::move(materialized);
    }
    const std::optional<quint64> rssAfterWarmup =
        processStatusKiB(QByteArrayLiteral("VmRSS:"));

    QVector<qint64> samples;
    samples.reserve(iterations);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        QElapsedTimer timer;
        timer.start();
        auto materialized = terminalMaterializeKittyImage(size, format, pixels);
        const qint64 elapsedNanoseconds = timer.nsecsElapsed();
        if (!validMaterialization(materialized, source, size)) {
            QTextStream(stderr) << "measured materialization " << iteration
                                << ": packed image did not match the source\n";
            return 1;
        }
        samples.append(elapsedNanoseconds);
        latest = std::move(materialized);
    }

    const std::optional<quint64> rssAfterMeasurement =
        processStatusKiB(QByteArrayLiteral("VmRSS:"));
    const std::optional<quint64> peakRss =
        processStatusKiB(QByteArrayLiteral("VmHWM:"));
    const TimingSummary timing = summarize(std::move(samples));
    const double elapsedSeconds =
        static_cast<double>(timing.totalNanoseconds) / 1'000'000'000.0;
    const double rawMiB = static_cast<double>(source.pixels.size())
        * static_cast<double>(iterations) / (1'024.0 * 1'024.0);
    const qsizetype packedBytes = latest->straightRgba.sizeInBytes();
    const double packedMiB = static_cast<double>(packedBytes)
        * static_cast<double>(iterations) / (1'024.0 * 1'024.0);
    const uchar *packedPixels = latest->straightRgba.constBits();
    const quint64 checksum = static_cast<quint64>(packedPixels[0])
        + static_cast<quint64>(packedPixels[packedBytes - 1]);

    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(2);
    output << "workload=kitty-materialization pixel_format=" << pixelFormat
           << " width=" << width << " height=" << height
           << " raw_frame_bytes=" << source.pixels.size()
           << " warmup=" << warmup << " iterations=" << iterations
           << " total_frames=" << totalFrames << '\n';
    output << "timing min_us=" << timing.minimumMicroseconds
           << " median_us=" << timing.medianMicroseconds
           << " p90_us=" << timing.percentile90Microseconds
           << " mean_us=" << timing.meanMicroseconds << " frames_per_second="
           << static_cast<double>(iterations) / elapsedSeconds
           << " raw_mib_per_second=" << rawMiB / elapsedSeconds
           << " packed_rgba_mib_per_second=" << packedMiB / elapsedSeconds
           << '\n';
    output << "materialization fully_opaque="
           << (latest->fullyOpaque ? "true" : "false")
           << " packed_rgba_bytes=" << packedBytes << " checksum=" << checksum
           << '\n';
    output << "memory rss_before_kib=";
    printOptionalKiB(output, rssBefore);
    output << " rss_after_warmup_kib=";
    printOptionalKiB(output, rssAfterWarmup);
    output << " rss_after_measurement_kib=";
    printOptionalKiB(output, rssAfterMeasurement);
    output << " peak_rss_kib=";
    printOptionalKiB(output, peakRss);
    if (rssAfterMeasurement.has_value() && rssBefore.has_value()) {
        output << " rss_total_delta_kib="
               << (*rssAfterMeasurement >= *rssBefore
                       ? *rssAfterMeasurement - *rssBefore
                       : quint64{0});
    } else {
        output << " rss_total_delta_kib=unavailable";
    }
    output << '\n';
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-kitty-graphics"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Sustained Kitty explicit-replacement import benchmark"));
    parser.addHelpOption();
    const QCommandLineOption widthOption(
        QStringLiteral("width"), QStringLiteral("Frame width in pixels."),
        QStringLiteral("pixels"), QStringLiteral("640"));
    const QCommandLineOption heightOption(
        QStringLiteral("height"), QStringLiteral("Frame height in pixels."),
        QStringLiteral("pixels"), QStringLiteral("360"));
    const QCommandLineOption warmupOption(
        QStringLiteral("warmup"), QStringLiteral("Unmeasured warmup frames."),
        QStringLiteral("count"), QStringLiteral("10"));
    const QCommandLineOption iterationsOption(
        QStringLiteral("iterations"), QStringLiteral("Measured frames."),
        QStringLiteral("count"), QStringLiteral("100"));
    const QCommandLineOption pixelFormatOption(
        QStringLiteral("pixel-format"),
        QStringLiteral("Kitty source format and alpha pattern: rgb24, rgba32, "
                       "rgba32-opaque, rgba32-first-translucent, or "
                       "rgba32-last-translucent. The materialization workload "
                       "also accepts gray8 and gray-alpha."),
        QStringLiteral("format"), QStringLiteral("rgb24"));
    const QCommandLineOption workloadOption(
        QStringLiteral("workload"),
        QStringLiteral("Benchmark the complete protocol path or isolated image "
                       "materialization: protocol or materialization."),
        QStringLiteral("name"), QStringLiteral("protocol"));
    parser.addOptions({widthOption, heightOption, warmupOption,
                       iterationsOption, pixelFormatOption, workloadOption});
    parser.process(application);

    const std::optional<int> width = integerOption(parser.value(widthOption));
    const std::optional<int> height = integerOption(parser.value(heightOption));
    const std::optional<int> warmup =
        integerOption(parser.value(warmupOption), true);
    const std::optional<int> iterations =
        integerOption(parser.value(iterationsOption));
    if (!width.has_value() || !height.has_value() || !warmup.has_value()
        || !iterations.has_value()) {
        QTextStream(stderr)
            << "width, height, and iterations must be positive; "
               "warmup must be non-negative\n";
        return 2;
    }
    const QString pixelFormat = parser.value(pixelFormatOption).toLower();
    const std::optional<SourcePattern> pattern = sourcePattern(pixelFormat);
    if (!pattern.has_value()) {
        QTextStream(stderr)
            << "pixel-format must be rgb24, rgba32, rgba32-opaque, "
               "rgba32-first-translucent, rgba32-last-translucent, gray8, "
               "or gray-alpha\n";
        return 2;
    }
    const std::optional<Workload> selectedWorkload =
        workload(parser.value(workloadOption).toLower());
    if (!selectedWorkload.has_value()) {
        QTextStream(stderr) << "workload must be protocol or materialization\n";
        return 2;
    }

    const quint64 totalFrames =
        static_cast<quint64>(*warmup) + static_cast<quint64>(*iterations);
    QString error;
    const std::optional<SourceFrame> source =
        makeSourceFrame(*width, *height, *pattern, &error);
    if (!source.has_value()) {
        QTextStream(stderr) << error << '\n';
        return 2;
    }
    if (*selectedWorkload == Workload::Materialization) {
        // This exits before creating a terminal, excluding protocol parsing,
        // Base64 decoding, and retained-frame publication from the samples.
        return runMaterializationBenchmark(*source, *pattern, pixelFormat,
                                           *width, *height, *warmup,
                                           *iterations, totalFrames);
    }
    const std::optional<int> kittyFormat = protocolFormat(*pattern);
    if (!kittyFormat.has_value()) {
        QTextStream(stderr) << "gray8 and gray-alpha are available only with "
                               "--workload materialization\n";
        return 2;
    }
    const std::optional<FrameCommands> commands =
        makeExplicitReplacementFrame(*source, *kittyFormat, *width, *height);
    GhosttyVtAdapter::Options options;
    options.geometry = {
        .columns = std::max(1, (*width + 9) / 10),
        .rows = std::max(1, (*height + 19) / 20),
        .cellWidthPixels = 10,
        .cellHeightPixels = 20,
    };
    if (commands->rawBytes > options.kittyImageStorageLimitBytes) {
        QTextStream(stderr)
            << "one frame exceeds the default image-storage-limit\n";
        return 2;
    }
    std::unique_ptr<GhosttyVtAdapter> adapter =
        GhosttyVtAdapter::create(options, {});
    if (adapter == nullptr) {
        QTextStream(stderr) << "failed to initialize libghostty adapter\n";
        return 1;
    }

    TerminalFrame frame;
    GhosttyVtAdapter::RenderSnapshot initial;
    if (adapter->renderFrame(&initial) != GhosttyVtAdapter::RenderResult::Ready
        || !applyTerminalUpdate(frame, initial.update)) {
        QTextStream(stderr) << "failed to initialize retained terminal frame\n";
        return 1;
    }

    const std::optional<quint64> rssBefore =
        processStatusKiB(QByteArrayLiteral("VmRSS:"));
    for (int iteration = 0; iteration < *warmup; ++iteration) {
        if (!importFrame(adapter.get(), *commands, *width, *height, &frame,
                         &error)) {
            QTextStream(stderr)
                << "warmup frame " << iteration << ": " << error << '\n';
            return 1;
        }
    }
    const std::optional<quint64> rssAfterWarmup =
        processStatusKiB(QByteArrayLiteral("VmRSS:"));

    QVector<qint64> samples;
    samples.reserve(*iterations);
    for (int iteration = 0; iteration < *iterations; ++iteration) {
        QElapsedTimer timer;
        timer.start();
        if (!importFrame(adapter.get(), *commands, *width, *height, &frame,
                         &error)) {
            QTextStream(stderr)
                << "measured frame " << iteration << ": " << error << '\n';
            return 1;
        }
        samples.append(timer.nsecsElapsed());
    }

    const std::optional<quint64> rssAfterMeasurement =
        processStatusKiB(QByteArrayLiteral("VmRSS:"));
    const std::optional<quint64> peakRss =
        processStatusKiB(QByteArrayLiteral("VmHWM:"));
    const TimingSummary timing = summarize(std::move(samples));
    const TerminalKittyGraphicsSnapshot &graphics = *frame.kittyGraphics;
    const TerminalKittyGraphicsImage &image =
        *graphics.placements.constFirst().image;
    const quint64 packedPlaneBytes =
        static_cast<quint64>(image.straightRgba.sizeInBytes());
    const quint64 formerTwoPlaneBytes =
        image.fullyOpaque ? packedPlaneBytes : packedPlaneBytes * 2ULL;
    const double elapsedSeconds =
        static_cast<double>(timing.totalNanoseconds) / 1'000'000'000.0;
    const double rawMiB = static_cast<double>(commands->rawBytes)
        * static_cast<double>(*iterations) / (1'024.0 * 1'024.0);
    const double wireMiB = static_cast<double>(commands->wireBytes)
        * static_cast<double>(*iterations) / (1'024.0 * 1'024.0);

    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(2);
    output << "protocol=kitty-" << pixelFormat
           << "-explicit-replacement width=" << *width << " height=" << *height
           << " raw_frame_bytes=" << commands->rawBytes
           << " wire_frame_bytes=" << commands->wireBytes
           << " packets_per_frame=" << commands->packets.size()
           << " warmup=" << *warmup << " iterations=" << *iterations
           << " total_frames=" << totalFrames << '\n';
    output << "timing min_us=" << timing.minimumMicroseconds
           << " median_us=" << timing.medianMicroseconds
           << " p90_us=" << timing.percentile90Microseconds
           << " mean_us=" << timing.meanMicroseconds << " frames_per_second="
           << static_cast<double>(*iterations) / elapsedSeconds
           << " raw_mib_per_second=" << rawMiB / elapsedSeconds
           << " wire_mib_per_second=" << wireMiB / elapsedSeconds << '\n';
    output << "snapshot placements=" << graphics.placements.size()
           << " packed_rgba_bytes=" << packedPlaneBytes
           << " former_two_plane_bytes=" << formerTwoPlaneBytes
           << " packed_bytes_saved=" << formerTwoPlaneBytes - packedPlaneBytes
           << " storage_generation=" << graphics.storageGeneration
           << " image_generation=" << image.generation << '\n';
    output << "memory rss_before_kib=";
    printOptionalKiB(output, rssBefore);
    output << " rss_after_warmup_kib=";
    printOptionalKiB(output, rssAfterWarmup);
    output << " rss_after_measurement_kib=";
    printOptionalKiB(output, rssAfterMeasurement);
    output << " peak_rss_kib=";
    printOptionalKiB(output, peakRss);
    if (rssAfterMeasurement.has_value() && rssBefore.has_value()) {
        output << " rss_total_delta_kib="
               << (*rssAfterMeasurement >= *rssBefore
                       ? *rssAfterMeasurement - *rssBefore
                       : quint64{0});
    } else {
        output << " rss_total_delta_kib=unavailable";
    }
    output << '\n';
    return 0;
}
