#include "ghostty_vt_adapter.h"

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
#include <limits>
#include <memory>
#include <numeric>
#include <optional>

namespace {

constexpr qsizetype kittyPayloadBytes = 4'096;
constexpr quint64 maximumDecodedFrameBytes = 400ULL * 1'024ULL * 1'024ULL;

struct FrameCommands {
    QVector<QByteArray> packets;
    quint64 rawBytes = 0;
    quint64 wireBytes = 0;
};

struct TimingSummary {
    double minimumMicroseconds = 0.0;
    double medianMicroseconds = 0.0;
    double percentile90Microseconds = 0.0;
    double meanMicroseconds = 0.0;
    qint64 totalNanoseconds = 0;
};

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

std::optional<quint64> checkedMultiply(quint64 left, quint64 right)
{
    if (right != 0 && left > std::numeric_limits<quint64>::max() / right) {
        return std::nullopt;
    }
    return left * right;
}

std::optional<FrameCommands> makeMpvShapedFrame(int width, int height,
                                                QString *error)
{
    const quint64 rawBytes =
        static_cast<quint64>(width) * static_cast<quint64>(height) * 3ULL;
    if (rawBytes > maximumDecodedFrameBytes
        || rawBytes
            > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        *error = QStringLiteral(
            "decoded RGB24 frame exceeds the Kitty 400 MiB limit");
        return std::nullopt;
    }

    QByteArray raw(static_cast<qsizetype>(rawBytes), Qt::Uninitialized);
    for (qsizetype index = 0; index < raw.size(); ++index) {
        raw[index] = static_cast<char>(
            (static_cast<quint64>(index) * 17ULL + 23ULL) % 251ULL);
    }
    const QByteArray encoded = raw.toBase64();

    FrameCommands result;
    result.rawBytes = rawBytes;
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
            packet = QByteArrayLiteral("\033[H\033_Ga=T,f=24,s=")
                + QByteArray::number(width) + QByteArrayLiteral(",v=")
                + QByteArray::number(height) + QByteArrayLiteral(",C=1,q=2,m=")
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
                 TerminalFrame *frame, QString *error)
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
        *error = QStringLiteral(
            "opaque replacement frames did not retain one placement");
        return false;
    }

    const TerminalKittyGraphicsPlacement &placement =
        frame->kittyGraphics->placements.constFirst();
    if (placement.image == nullptr || !placement.image->fullyOpaque
        || !placement.image->alphaPlane.isNull()) {
        *error = QStringLiteral(
            "RGB24 frame did not use the opaque single-plane path");
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

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-kitty-graphics"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Sustained mpv-shaped Kitty RGB24 import benchmark"));
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
    parser.addOptions(
        {widthOption, heightOption, warmupOption, iterationsOption});
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

    const quint64 totalFrames =
        static_cast<quint64>(*warmup) + static_cast<quint64>(*iterations);
    QString error;
    const std::optional<FrameCommands> commands =
        makeMpvShapedFrame(*width, *height, &error);
    if (!commands.has_value()) {
        QTextStream(stderr) << error << '\n';
        return 2;
    }
    const std::optional<quint64> totalRawBytes =
        checkedMultiply(commands->rawBytes, totalFrames);
    const std::optional<quint64> pixelsPerFrame = checkedMultiply(
        static_cast<quint64>(*width), static_cast<quint64>(*height));
    const std::optional<quint64> uncullBytesPerFrame =
        pixelsPerFrame.has_value() ? checkedMultiply(*pixelsPerFrame, 8ULL)
                                   : std::nullopt;
    const std::optional<quint64> preChangeUnculledQtBytes =
        uncullBytesPerFrame.has_value()
        ? checkedMultiply(*uncullBytesPerFrame, totalFrames)
        : std::nullopt;
    if (!totalRawBytes.has_value() || !preChangeUnculledQtBytes.has_value()) {
        QTextStream(stderr)
            << "frame count and dimensions overflow byte metrics\n";
        return 2;
    }

    GhosttyVtAdapter::Options options;
    options.geometry = {
        .columns = std::max(1, (*width + 9) / 10),
        .rows = std::max(1, (*height + 19) / 20),
        .cellWidthPixels = 10,
        .cellHeightPixels = 20,
    };
    if (*totalRawBytes > options.kittyImageStorageLimitBytes) {
        QTextStream(stderr)
            << "selected frame count exceeds the default image-storage-limit; "
               "reduce dimensions or iterations so eviction cannot hide "
               "snapshot growth\n";
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
        if (!importFrame(adapter.get(), *commands, &frame, &error)) {
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
        if (!importFrame(adapter.get(), *commands, &frame, &error)) {
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
    const quint64 rgbPlaneBytes =
        static_cast<quint64>(image.straightRgbPlane.sizeInBytes());
    const quint64 alphaPlaneBytes =
        static_cast<quint64>(image.alphaPlane.sizeInBytes());
    const quint64 estimatedRetainedLibghosttyRawBytes =
        std::min(*totalRawBytes, options.kittyImageStorageLimitBytes);
    const double elapsedSeconds =
        static_cast<double>(timing.totalNanoseconds) / 1'000'000'000.0;
    const double rawMiB = static_cast<double>(commands->rawBytes)
        * static_cast<double>(*iterations) / (1'024.0 * 1'024.0);
    const double wireMiB = static_cast<double>(commands->wireBytes)
        * static_cast<double>(*iterations) / (1'024.0 * 1'024.0);

    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(2);
    output << "protocol=kitty-rgb24-mpv-shaped width=" << *width
           << " height=" << *height << " raw_frame_bytes=" << commands->rawBytes
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
           << " rgb_plane_bytes=" << rgbPlaneBytes
           << " alpha_plane_bytes=" << alphaPlaneBytes
           << " total_plane_bytes=" << rgbPlaneBytes + alphaPlaneBytes
           << " estimated_libghostty_raw_bytes="
           << estimatedRetainedLibghosttyRawBytes
           << " pre_change_unculled_qt_cpu_plane_bytes="
           << *preChangeUnculledQtBytes
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
