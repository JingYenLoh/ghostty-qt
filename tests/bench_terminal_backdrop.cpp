#include "terminal_backdrop_p.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <utility>

namespace {

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

QImage makeSource(int width, int height)
{
    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull()) return {};
    image.setDevicePixelRatio(1.0);
    for (int y = 0; y < height; ++y) {
        uchar *const line = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const qsizetype offset = static_cast<qsizetype>(x) * 4;
            line[offset] = static_cast<uchar>((x * 17 + y * 7 + 23) % 251);
            line[offset + 1] = static_cast<uchar>((x * 11 + y * 29 + 47) % 251);
            line[offset + 2] = static_cast<uchar>((x * 31 + y * 13 + 71) % 251);
            line[offset + 3] = static_cast<uchar>((x * 19 + y * 37 + 31) % 251);
        }
    }
    return image;
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

struct LegacyPlanes {
    QImage rgb;
    QImage alpha;
};

std::optional<LegacyPlanes> prepareLegacyPlanes(const QImage &source)
{
    QImage rgb = source.convertToFormat(QImage::Format_RGBA8888);
    QImage alpha(rgb.size(), QImage::Format_RGBX8888);
    if (rgb.isNull() || alpha.isNull()) return std::nullopt;
    rgb.detach();
    rgb.setDevicePixelRatio(1.0);
    alpha.setDevicePixelRatio(1.0);
    for (int y = 0; y < rgb.height(); ++y) {
        uchar *const rgbLine = rgb.scanLine(y);
        uchar *const alphaLine = alpha.scanLine(y);
        for (int x = 0; x < rgb.width(); ++x) {
            const qsizetype offset = static_cast<qsizetype>(x) * 4;
            const uchar value = rgbLine[offset + 3];
            alphaLine[offset] = value;
            alphaLine[offset + 1] = value;
            alphaLine[offset + 2] = value;
            alphaLine[offset + 3] = 255;
            rgbLine[offset + 3] = 255;
        }
    }
    if (!rgb.reinterpretAsFormat(QImage::Format_RGBX8888)) {
        return std::nullopt;
    }
    return LegacyPlanes{.rgb = std::move(rgb), .alpha = std::move(alpha)};
}

bool measurePackedPreparation(const QImage &source, qint64 *nanoseconds,
                              quint64 *checksum)
{
    QImage decoded = source.copy();
    if (decoded.isNull()) return false;
    QElapsedTimer timer;
    timer.start();
    auto asset = prepareTerminalBackgroundImage(std::move(decoded), 1);
    *nanoseconds = timer.nsecsElapsed();
    if (!asset || asset->straightRgba.size() != source.size()
        || asset->straightRgba.format() != QImage::Format_RGBA8888) {
        return false;
    }
    *checksum += static_cast<quint64>(asset->straightRgba.pixel(
        asset->straightRgba.width() / 2, asset->straightRgba.height() / 2));
    return true;
}

bool measureLegacyPreparation(const QImage &source, qint64 *nanoseconds,
                              quint64 *checksum)
{
    const QImage decoded = source.copy();
    if (decoded.isNull()) return false;
    QElapsedTimer timer;
    timer.start();
    auto planes = prepareLegacyPlanes(decoded);
    *nanoseconds = timer.nsecsElapsed();
    if (!planes || planes->rgb.size() != source.size()
        || planes->alpha.size() != source.size()) {
        return false;
    }
    const QPoint center(planes->rgb.width() / 2, planes->rgb.height() / 2);
    *checksum += static_cast<quint64>(planes->rgb.pixel(center));
    *checksum += static_cast<quint64>(planes->alpha.pixel(center));
    return true;
}

bool measureComposition(const TerminalBackgroundImageAsset &asset,
                        qint64 *nanoseconds, quint64 *checksum)
{
    QElapsedTimer timer;
    timer.start();
    const QImage result = terminalCompositedBackgroundImage(
        asset, QColor::fromRgb(23, 41, 67), 211, 0.73);
    *nanoseconds = timer.nsecsElapsed();
    if (result.isNull() || result.size() != asset.straightRgba.size()
        || result.format() != QImage::Format_ARGB32_Premultiplied) {
        return false;
    }
    const QRgb sample = result.pixel(result.width() / 2, result.height() / 2);
    *checksum += static_cast<quint64>(sample);
    return true;
}

void printTiming(QTextStream &output, QStringView workload,
                 const TimingSummary &timing, int iterations,
                 double measuredPixels)
{
    const double elapsedSeconds =
        static_cast<double>(timing.totalNanoseconds) / 1'000'000'000.0;
    output << "workload=" << workload
           << " min_us=" << timing.minimumMicroseconds
           << " median_us=" << timing.medianMicroseconds
           << " p90_us=" << timing.percentile90Microseconds
           << " mean_us=" << timing.meanMicroseconds
           << " operations_per_second="
           << static_cast<double>(iterations) / elapsedSeconds
           << " megapixels_per_second="
           << measuredPixels / elapsedSeconds / 1'000'000.0 << '\n';
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("bench-terminal-backdrop"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Background-image preparation and reference composition benchmark"));
    parser.addHelpOption();
    const QCommandLineOption widthOption(
        QStringLiteral("width"), QStringLiteral("Image width in pixels."),
        QStringLiteral("pixels"), QStringLiteral("640"));
    const QCommandLineOption heightOption(
        QStringLiteral("height"), QStringLiteral("Image height in pixels."),
        QStringLiteral("pixels"), QStringLiteral("360"));
    const QCommandLineOption warmupOption(
        QStringLiteral("warmup"), QStringLiteral("Unmeasured iterations."),
        QStringLiteral("count"), QStringLiteral("10"));
    const QCommandLineOption iterationsOption(
        QStringLiteral("iterations"), QStringLiteral("Measured iterations."),
        QStringLiteral("count"), QStringLiteral("100"));
    const QCommandLineOption sourceFormatOption(
        QStringLiteral("source-format"),
        QStringLiteral("Decoded source format: rgba8888 or argb32."),
        QStringLiteral("format"), QStringLiteral("rgba8888"));
    parser.addOptions({widthOption, heightOption, warmupOption,
                       iterationsOption, sourceFormatOption});
    parser.process(application);

    const std::optional<int> width = integerOption(parser.value(widthOption));
    const std::optional<int> height = integerOption(parser.value(heightOption));
    const std::optional<int> warmup =
        integerOption(parser.value(warmupOption), true);
    const std::optional<int> iterations =
        integerOption(parser.value(iterationsOption));
    if (!width || !height || !warmup || !iterations) {
        QTextStream(stderr)
            << "width, height, and iterations must be positive; "
               "warmup must be non-negative\n";
        return 2;
    }

    const QString sourceFormat = parser.value(sourceFormatOption).toLower();
    if (sourceFormat != QStringLiteral("rgba8888")
        && sourceFormat != QStringLiteral("argb32")) {
        QTextStream(stderr) << "source-format must be rgba8888 or argb32\n";
        return 2;
    }
    QImage source = makeSource(*width, *height);
    if (sourceFormat == QStringLiteral("argb32")) {
        source = source.convertToFormat(QImage::Format_ARGB32);
    }
    if (source.isNull()) {
        QTextStream(stderr) << "could not allocate source image\n";
        return 1;
    }

    QImage initialDecoded = source.copy();
    auto prepared =
        prepareTerminalBackgroundImage(std::move(initialDecoded), 1);
    if (!prepared) {
        QTextStream(stderr) << prepared.error() << '\n';
        return 1;
    }

    quint64 packedChecksum = 0;
    quint64 legacyChecksum = 0;
    quint64 compositionChecksum = 0;
    for (int iteration = 0; iteration < *warmup; ++iteration) {
        qint64 ignored = 0;
        if (!measurePackedPreparation(source, &ignored, &packedChecksum)
            || !measureLegacyPreparation(source, &ignored, &legacyChecksum)
            || !measureComposition(*prepared, &ignored, &compositionChecksum)) {
            QTextStream(stderr) << "warmup iteration failed\n";
            return 1;
        }
    }
    packedChecksum = 0;
    legacyChecksum = 0;
    compositionChecksum = 0;

    QVector<qint64> packedSamples;
    QVector<qint64> legacySamples;
    QVector<qint64> compositionSamples;
    packedSamples.reserve(*iterations);
    legacySamples.reserve(*iterations);
    compositionSamples.reserve(*iterations);
    for (int iteration = 0; iteration < *iterations; ++iteration) {
        qint64 packedNanoseconds = 0;
        qint64 legacyNanoseconds = 0;
        const bool packedFirst = iteration % 2 == 0;
        const bool preparationSucceeded = packedFirst
            ? measurePackedPreparation(source, &packedNanoseconds,
                                       &packedChecksum)
                && measureLegacyPreparation(source, &legacyNanoseconds,
                                            &legacyChecksum)
            : measureLegacyPreparation(source, &legacyNanoseconds,
                                       &legacyChecksum)
                && measurePackedPreparation(source, &packedNanoseconds,
                                            &packedChecksum);
        qint64 compositionNanoseconds = 0;
        if (!preparationSucceeded
            || !measureComposition(*prepared, &compositionNanoseconds,
                                   &compositionChecksum)) {
            QTextStream(stderr) << "measured iteration failed\n";
            return 1;
        }
        packedSamples.append(packedNanoseconds);
        legacySamples.append(legacyNanoseconds);
        compositionSamples.append(compositionNanoseconds);
    }

    const TimingSummary packedTiming = summarize(std::move(packedSamples));
    const TimingSummary legacyTiming = summarize(std::move(legacySamples));
    const TimingSummary compositionTiming =
        summarize(std::move(compositionSamples));
    const quint64 packedBytes =
        static_cast<quint64>(prepared->straightRgba.sizeInBytes());
    const quint64 formerTwoPlaneBytes = packedBytes * 2ULL;
    const double pixels = static_cast<double>(*width)
        * static_cast<double>(*height) * static_cast<double>(*iterations);

    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(2);
    output << "source_format=" << sourceFormat << " width=" << *width
           << " height=" << *height << " warmup=" << *warmup
           << " iterations=" << *iterations << '\n';
    printTiming(output, QStringLiteral("packed-asset-prepare"), packedTiming,
                *iterations, pixels);
    printTiming(output, QStringLiteral("former-two-plane-prepare"),
                legacyTiming, *iterations, pixels);
    printTiming(output, QStringLiteral("packed-asset-cpu-compose"),
                compositionTiming, *iterations, pixels);
    output << "comparison prepare_mean_speedup="
           << legacyTiming.meanMicroseconds
            / std::max(packedTiming.meanMicroseconds, 0.000'001)
           << " prepare_mean_time_reduction_percent="
           << (1.0
               - packedTiming.meanMicroseconds / legacyTiming.meanMicroseconds)
            * 100.0
           << '\n';
    output << "storage asset_packed_rgba_logical_bytes=" << packedBytes
           << " former_two_plane_bytes=" << formerTwoPlaneBytes
           << " packed_bytes_saved=" << formerTwoPlaneBytes - packedBytes
           << " packed_percent_saved=50.00"
           << " packed_checksum=" << packedChecksum
           << " legacy_checksum=" << legacyChecksum
           << " composition_checksum=" << compositionChecksum << '\n';
    return 0;
}
