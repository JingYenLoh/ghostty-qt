#include "terminal_backdrop.h"
#include "terminal_backdrop_p.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace {

struct DecodedImageKey {
    QString path;
    qint64 size = -1;
    qint64 modifiedMilliseconds = 0;

    bool operator==(const DecodedImageKey &) const = default;
};

size_t qHash(const DecodedImageKey &key, size_t seed = 0) noexcept
{
    return qHashMulti(seed, key.path, key.size, key.modifiedMilliseconds);
}

struct Waiter {
    QPointer<QObject> receiver;
    std::stop_token stopToken;
    TerminalBackgroundImageCallback callback;
};

struct PendingLoad {
    std::vector<Waiter> waiters;
};

struct DecodeFailure {
    QString message;
    std::chrono::steady_clock::time_point retryAfter;
};

QMutex cacheMutex;
QHash<DecodedImageKey,
      std::weak_ptr<const TerminalBackgroundImageAsset>>
    decodedImages;
QHash<DecodedImageKey, DecodeFailure> decodeFailures;
QHash<DecodedImageKey, std::shared_ptr<PendingLoad>> pendingLoads;
std::atomic<quint64> nextAssetSerial{1};
qsizetype decodedCacheRequestsUntilSweep = 0;

QThreadPool &backgroundImageThreadPool()
{
    static const auto pool = [] {
        auto result = std::make_unique<QThreadPool>();
        result->setMaxThreadCount(2);
        result->setExpiryTimeout(30'000);
        return result;
    }();
    return *pool;
}

qreal alignedOffset(qreal available,
                    TerminalBackgroundImagePosition position,
                    bool horizontal) noexcept
{
    constexpr auto positionCount =
        std::to_underlying(TerminalBackgroundImagePosition::BottomRight) + 1;
    auto index = std::to_underlying(position);
    if (index >= positionCount) {
        index = std::to_underlying(TerminalBackgroundImagePosition::Center);
    }
    const auto alignment = horizontal ? index % 3 : index / 3;
    return available * static_cast<qreal>(alignment) / 2.0;
}

std::expected<TerminalBackgroundImageAsset, QString>
prepareStraightRgba(QImage source, quint64 serial)
{
    if (source.isNull()) {
        return std::unexpected(
            QStringLiteral("Decoded background image is empty."));
    }

    QImage rgba;
    if (source.format() == QImage::Format_RGBA8888) {
        source.detach();
        rgba = std::move(source);
    } else {
        rgba = source.convertToFormat(QImage::Format_RGBA8888);
    }
    if (rgba.isNull()) {
        return std::unexpected(
            QStringLiteral("Could not allocate background image pixels."));
    }
    rgba.setDevicePixelRatio(1.0);

    return TerminalBackgroundImageAsset{
        .straightRgba = std::move(rgba),
        .serial = serial,
    };
}

std::expected<std::shared_ptr<const TerminalBackgroundImageAsset>, QString>
decodeImageFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(
            QStringLiteral("Could not open background image '%1': %2")
                .arg(path, file.errorString()));
    }

    QByteArray format = QImageReader::imageFormat(&file).toLower();
    if (format.isEmpty()) {
        format = QFileInfo(path).suffix().toLatin1().toLower();
    }
    if (format == QByteArrayLiteral("jpg")) {
        format = QByteArrayLiteral("jpeg");
    }
    if (format != QByteArrayLiteral("png")
        && format != QByteArrayLiteral("jpeg")) {
        return std::unexpected(
            QStringLiteral("Background image '%1' is not PNG or JPEG.")
                .arg(path));
    }

    if (!file.seek(0)) {
        return std::unexpected(
            QStringLiteral("Could not read background image '%1': %2")
                .arg(path, file.errorString()));
    }
    QImageReader reader(&file, format);
    reader.setAutoTransform(false);
    QImage image = reader.read();
    if (image.isNull()) {
        return std::unexpected(
            QStringLiteral("Could not decode background image '%1': %2")
                .arg(path, reader.errorString()));
    }

    auto asset = prepareTerminalBackgroundImage(
        std::move(image),
        nextAssetSerial.fetch_add(1, std::memory_order_relaxed));
    if (!asset) {
        return std::unexpected(
            QStringLiteral("Could not prepare background image '%1': %2")
                .arg(path, std::move(asset.error())));
    }
    return std::make_shared<const TerminalBackgroundImageAsset>(
        std::move(*asset));
}

void deliver(std::vector<Waiter> waiters,
             TerminalBackgroundImageResult result)
{
    QObject *const dispatcher = QCoreApplication::instance();
    if (dispatcher == nullptr) return;
    QMetaObject::invokeMethod(
        dispatcher,
        [waiters = std::move(waiters), result = std::move(result)]() mutable {
            for (Waiter &waiter : waiters) {
                if (!waiter.stopToken.stop_requested()
                    && waiter.receiver != nullptr && waiter.callback) {
                    waiter.callback(result);
                }
            }
        },
        Qt::QueuedConnection);
}

bool hasActiveWaiter(const PendingLoad &load) noexcept
{
    return std::ranges::any_of(load.waiters, [](const Waiter &waiter) {
        return !waiter.stopToken.stop_requested();
    });
}

void rememberFailure(const DecodedImageKey &key, QString message)
{
    decodeFailures.insert(
        key,
        {
            .message = std::move(message),
            .retryAfter = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(250),
        });
    while (decodeFailures.size() > 64) {
        decodeFailures.erase(decodeFailures.begin());
    }
}

void sweepExpiredDecodedImages()
{
    // Active split-heavy workloads should not scan every live weak entry for
    // every request. A stale entry is tiny and survives at most 31 subsequent
    // lookups; inserting the same key still replaces it immediately.
    if (decodedCacheRequestsUntilSweep > 0) {
        --decodedCacheRequestsUntilSweep;
        return;
    }
    decodedImages.removeIf(
        [](auto iterator) { return iterator.value().expired(); });
    decodedCacheRequestsUntilSweep = 31;
}

} // namespace

std::expected<TerminalBackgroundImageAsset, QString>
prepareTerminalBackgroundImage(QImage source, quint64 serial)
{
    return prepareStraightRgba(std::move(source), serial);
}

QRectF terminalBackgroundImagePlacement(
    const QRectF &viewport, const QSize &sourcePixels, qreal devicePixelRatio,
    TerminalBackgroundImageFit fit,
    TerminalBackgroundImagePosition position) noexcept
{
    if (viewport.isEmpty() || sourcePixels.width() <= 0
        || sourcePixels.height() <= 0 || !std::isfinite(devicePixelRatio)
        || devicePixelRatio <= 0.0) {
        return {};
    }

    QSizeF target;
    switch (fit) {
    case TerminalBackgroundImageFit::Stretch:
        target = viewport.size();
        break;
    case TerminalBackgroundImageFit::None:
        target = QSizeF(sourcePixels) / devicePixelRatio;
        break;
    case TerminalBackgroundImageFit::Contain:
    case TerminalBackgroundImageFit::Cover: {
        const qreal horizontal =
            viewport.width() / static_cast<qreal>(sourcePixels.width());
        const qreal vertical =
            viewport.height() / static_cast<qreal>(sourcePixels.height());
        const qreal scale = fit == TerminalBackgroundImageFit::Contain
            ? std::min(horizontal, vertical)
            : std::max(horizontal, vertical);
        target = QSizeF(sourcePixels) * scale;
        break;
    }
    }

    const qreal availableX = viewport.width() - target.width();
    const qreal availableY = viewport.height() - target.height();
    return QRectF(viewport.x() + alignedOffset(availableX, position, true),
                  viewport.y() + alignedOffset(availableY, position, false),
                  target.width(), target.height());
}

QImage terminalCompositedBackgroundImage(
    const TerminalBackgroundImageAsset &asset,
    const QColor &opaqueBackground, quint8 backgroundAlpha,
    double imageOpacity)
{
    const QImage &rgba = asset.straightRgba;
    if (rgba.isNull() || rgba.format() != QImage::Format_RGBA8888
        || !opaqueBackground.isValid() || !std::isfinite(imageOpacity)) {
        return {};
    }

    QImage result(rgba.size(), QImage::Format_ARGB32_Premultiplied);
    result.setDevicePixelRatio(1.0);
    if (result.isNull()) return {};

    const double globalAlpha =
        static_cast<double>(backgroundAlpha) / 255.0;
    if (globalAlpha <= 0.0) {
        result.fill(Qt::transparent);
        return result;
    }

    const double multiplier = std::min(imageOpacity, 1.0 / globalAlpha);
    const std::array<double, 3> background{
        opaqueBackground.redF(),
        opaqueBackground.greenF(),
        opaqueBackground.blueF(),
    };
    const auto byte = [](double value) {
        return static_cast<int>(
            std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    };

    for (int y = 0; y < rgba.height(); ++y) {
        const uchar *sourceLine = rgba.constScanLine(y);
        auto *destination = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < rgba.width(); ++x) {
            const qsizetype offset = 4 * static_cast<qsizetype>(x);
            const double sourceAlpha = sourceLine[offset + 3] / 255.0;
            const double weightedAlpha = sourceAlpha * multiplier;
            const double backgroundWeight =
                std::max(0.0, 1.0 - weightedAlpha);
            const double finalAlpha =
                globalAlpha * (weightedAlpha + backgroundWeight);
            destination[x] =
                qRgba(byte(globalAlpha
                           * (sourceLine[offset] / 255.0 * weightedAlpha
                              + background[0] * backgroundWeight)),
                      byte(globalAlpha
                           * (sourceLine[offset + 1] / 255.0 * weightedAlpha
                              + background[1] * backgroundWeight)),
                      byte(globalAlpha
                           * (sourceLine[offset + 2] / 255.0 * weightedAlpha
                              + background[2] * backgroundWeight)),
                      byte(finalAlpha));
        }
    }
    return result;
}

QImage terminalCompositedBackgroundImage(
    const QImage &source, const QColor &opaqueBackground,
    quint8 backgroundAlpha, double imageOpacity)
{
    auto asset = prepareTerminalBackgroundImage(source, 0);
    return asset
        ? terminalCompositedBackgroundImage(
              *asset, opaqueBackground, backgroundAlpha, imageOpacity)
        : QImage{};
}

TerminalBackgroundImageRequestHandle requestTerminalBackgroundImage(
    const TerminalBackgroundImageRequest &request, QObject *receiver,
    TerminalBackgroundImageCallback callback)
{
    if (receiver == nullptr || !callback || request.source.path.isEmpty()) {
        return {};
    }

    std::stop_source stopSource;
    const std::stop_token stopToken = stopSource.get_token();
    TerminalBackgroundImageRequestHandle handle(std::move(stopSource));
    Waiter waiter{
        .receiver = QPointer<QObject>(receiver),
        .stopToken = stopToken,
        .callback = std::move(callback),
    };

    backgroundImageThreadPool().start(
        [source = request.source, waiter = std::move(waiter)]() mutable {
            if (waiter.stopToken.stop_requested()) return;

            // File-system metadata may block for network or FUSE paths, so it
            // belongs on the same bounded worker pool as decoding.
            const QFileInfo sourceInfo(source.path);
            const DecodedImageKey key{
                .path = source.path,
                .size = sourceInfo.size(),
                .modifiedMilliseconds =
                    sourceInfo.lastModified().toMSecsSinceEpoch(),
            };

            std::shared_ptr<const TerminalBackgroundImageAsset> cached;
            std::optional<QString> cachedFailure;
            std::shared_ptr<PendingLoad> load;
            {
                QMutexLocker locker(&cacheMutex);
                sweepExpiredDecodedImages();
                if (auto image = decodedImages.value(key).lock()) {
                    cached = std::move(image);
                } else if (auto failure = decodeFailures.find(key);
                           failure != decodeFailures.end()) {
                    if (std::chrono::steady_clock::now()
                        < failure->retryAfter) {
                        cachedFailure = failure->message;
                    } else {
                        decodeFailures.erase(failure);
                    }
                }

                if (!cached && !cachedFailure) {
                    if (const auto pending = pendingLoads.constFind(key);
                        pending != pendingLoads.cend()) {
                        (*pending)->waiters.push_back(std::move(waiter));
                        return;
                    }
                    load = std::make_shared<PendingLoad>();
                    load->waiters.push_back(std::move(waiter));
                    pendingLoads.insert(key, load);
                }
            }

            if (cached || cachedFailure) {
                std::vector<Waiter> waiters;
                waiters.push_back(std::move(waiter));
                if (cached) {
                    deliver(std::move(waiters), std::move(cached));
                } else {
                    deliver(std::move(waiters),
                            std::unexpected(
                                std::move(*cachedFailure)));
                }
                return;
            }

            {
                QMutexLocker locker(&cacheMutex);
                if (!hasActiveWaiter(*load)) {
                    if (pendingLoads.value(key) == load) {
                        pendingLoads.remove(key);
                    }
                    return;
                }
            }

            TerminalBackgroundImageResult result =
                decodeImageFile(key.path);
            std::vector<Waiter> waiters;
            {
                QMutexLocker locker(&cacheMutex);
                if (pendingLoads.value(key) != load) return;
                pendingLoads.remove(key);
                waiters = std::move(load->waiters);
                if (result) {
                    decodedImages.insert(key, *result);
                    decodeFailures.remove(key);
                } else {
                    rememberFailure(key, result.error());
                }
            }
            deliver(std::move(waiters), std::move(result));
        });
    return handle;
}
