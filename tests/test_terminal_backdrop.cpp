#include "terminal/rendering/terminal_backdrop_p.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>

namespace {

constexpr auto asyncTimeout = 3'000;

struct ImageReply {
    std::optional<TerminalBackgroundImageResult> result;
    QThread *callbackThread = nullptr;
    int callbackCount = 0;
    bool callbackWasQueued = false;
};

TerminalBackgroundImageRequest imageRequest(const QString &path)
{
    return {
        .source =
            {
                .path = path,
                .optional = false,
            },
    };
}

TerminalBackgroundImageCallback
captureReply(ImageReply &reply, const bool *requestReturned = nullptr)
{
    return [&reply, requestReturned](TerminalBackgroundImageResult result) {
        ++reply.callbackCount;
        reply.callbackThread = QThread::currentThread();
        reply.callbackWasQueued =
            requestReturned == nullptr || *requestReturned;
        reply.result.emplace(std::move(result));
    };
}

QString replyFailure(const ImageReply &reply)
{
    if (!reply.result.has_value()) {
        return QStringLiteral("Background image callback did not run.");
    }
    return reply.result->has_value() ? QString{} : reply.result->error();
}

QByteArray packedPixels(const QImage &image)
{
    QByteArray result;
    const qsizetype lineBytes = static_cast<qsizetype>(image.width()) * 4;
    result.reserve(lineBytes * image.height());
    for (int y = 0; y < image.height(); ++y) {
        result.append(reinterpret_cast<const char *>(image.constScanLine(y)),
                      lineBytes);
    }
    return result;
}

QByteArray expectedPackedBytes(const QImage &source)
{
    const QImage straight = source.convertToFormat(QImage::Format_RGBA8888);
    return packedPixels(straight);
}

QImage patternedImage(QSize size)
{
    QImage result(size, QImage::Format_RGBA8888);
    for (int y = 0; y < result.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            line[x] = qRgba((17 * x + 29 * y) % 256, (53 * x + 11 * y) % 256,
                            (7 * x + 97 * y) % 256, (31 * x + 43 * y) % 256);
        }
    }
    return result;
}

QByteArray encodedPng(const QImage &image)
{
    QByteArray result;
    QBuffer buffer(&result);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        return {};
    }
    return result;
}

void equalizeByteLengths(QByteArray &first, QByteArray &second)
{
    const qsizetype size = std::max(first.size(), second.size());
    first.append(QByteArray(size - first.size(), '\0'));
    second.append(QByteArray(size - second.size(), '\0'));
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size() && file.flush();
}

bool setModificationTime(const QString &path, qint64 seconds,
                         qint64 nanoseconds)
{
    const QByteArray nativePath = QFile::encodeName(path);
    const std::array<timespec, 2> times{
        timespec{.tv_sec = 0, .tv_nsec = UTIME_OMIT},
        timespec{
            .tv_sec = static_cast<time_t>(seconds),
            .tv_nsec = static_cast<long>(nanoseconds),
        },
    };
    return ::utimensat(AT_FDCWD, nativePath.constData(), times.data(), 0) == 0;
}

std::optional<struct stat> fileStatus(const QString &path)
{
    struct stat result{};
    const QByteArray nativePath = QFile::encodeName(path);
    if (::stat(nativePath.constData(), &result) != 0) return std::nullopt;
    return result;
}

bool replaceAtomically(const QString &source, const QString &destination)
{
    const QByteArray nativeSource = QFile::encodeName(source);
    const QByteArray nativeDestination = QFile::encodeName(destination);
    return ::rename(nativeSource.constData(), nativeDestination.constData())
        == 0;
}

QImage solidImage(const QColor &color, QSize size = QSize(3, 2))
{
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(color);
    return image;
}

} // namespace

class TerminalBackdropTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void placesEveryFitInPhysicalSpace();
    void anchorsEveryPosition_data();
    void anchorsEveryPosition();
    void composesGhosttyOpacityExactly();
    void decodesPngIntoPackedRgba();
    void decodesJpegIntoPackedRgba();
    void rejectsUnsupportedAndCorruptImages();
    void rejectsSpecialFilesWithoutStarvingPool();
    void repairedImageBypassesNegativeCache();
    void coalescesIdenticalInflightRequests();
    void cancellationSuppressesCallback();
    void destroyedReceiverSuppressesCallback();
    void fingerprintsReplacementsAndReusesLiveCache();
    void detectsSameMillisecondInPlaceRewrite();
    void detectsAtomicReplacementWithIdenticalMetadata();
    void decodesOpenedFileAcrossAtomicReplacement();
    void retriesInPlaceRewriteAfterOpen();
    void deliversCallbacksOnApplicationThread();

private:
    [[nodiscard]] std::unique_ptr<QTemporaryDir> makeTestDirectory() const;

    QString temporaryRoot_;
};

void TerminalBackdropTest::initTestCase()
{
    temporaryRoot_ = QDir::current().filePath(QStringLiteral("tmp"));
    QVERIFY2(
        QDir().mkpath(temporaryRoot_),
        qPrintable(QStringLiteral("Could not create test-owned directory %1")
                       .arg(temporaryRoot_)));
}

std::unique_ptr<QTemporaryDir> TerminalBackdropTest::makeTestDirectory() const
{
    return std::make_unique<QTemporaryDir>(
        QDir(temporaryRoot_)
            .filePath(QStringLiteral("terminal-backdrop-XXXXXX")));
}

void TerminalBackdropTest::placesEveryFitInPhysicalSpace()
{
    const QRectF viewport(0, 0, 100, 100);
    const QSize source(200, 100);

    QCOMPARE(terminalBackgroundImagePlacement(
                 viewport, source, 1.0, TerminalBackgroundImageFit::Contain,
                 TerminalBackgroundImagePosition::Center),
             QRectF(0, 25, 100, 50));
    QCOMPARE(terminalBackgroundImagePlacement(
                 viewport, source, 1.0, TerminalBackgroundImageFit::Cover,
                 TerminalBackgroundImagePosition::Center),
             QRectF(-50, 0, 200, 100));
    QCOMPARE(terminalBackgroundImagePlacement(
                 viewport, source, 1.0, TerminalBackgroundImageFit::Stretch,
                 TerminalBackgroundImagePosition::Center),
             viewport);
    QCOMPARE(terminalBackgroundImagePlacement(
                 viewport, source, 2.0, TerminalBackgroundImageFit::None,
                 TerminalBackgroundImagePosition::Center),
             QRectF(0, 25, 100, 50));
}

void TerminalBackdropTest::anchorsEveryPosition_data()
{
    QTest::addColumn<TerminalBackgroundImagePosition>("position");
    QTest::addColumn<QPointF>("topLeft");

    QTest::newRow("top-left")
        << TerminalBackgroundImagePosition::TopLeft << QPointF(0, 0);
    QTest::newRow("top-center")
        << TerminalBackgroundImagePosition::TopCenter << QPointF(40, 0);
    QTest::newRow("top-right")
        << TerminalBackgroundImagePosition::TopRight << QPointF(80, 0);
    QTest::newRow("center-left")
        << TerminalBackgroundImagePosition::CenterLeft << QPointF(0, 45);
    QTest::newRow("center")
        << TerminalBackgroundImagePosition::Center << QPointF(40, 45);
    QTest::newRow("center-right")
        << TerminalBackgroundImagePosition::CenterRight << QPointF(80, 45);
    QTest::newRow("bottom-left")
        << TerminalBackgroundImagePosition::BottomLeft << QPointF(0, 90);
    QTest::newRow("bottom-center")
        << TerminalBackgroundImagePosition::BottomCenter << QPointF(40, 90);
    QTest::newRow("bottom-right")
        << TerminalBackgroundImagePosition::BottomRight << QPointF(80, 90);
}

void TerminalBackdropTest::anchorsEveryPosition()
{
    QFETCH(TerminalBackgroundImagePosition, position);
    QFETCH(QPointF, topLeft);
    const auto placement = terminalBackgroundImagePlacement(
        QRectF(0, 0, 100, 100), QSize(20, 10), 1.0,
        TerminalBackgroundImageFit::None, position);
    QCOMPARE(placement.topLeft(), topLeft);
}

void TerminalBackdropTest::composesGhosttyOpacityExactly()
{
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    source.setPixelColor(1, 0, QColor(0, 255, 0, 0));
    const QImage original = source.copy();
    const QColor background(0, 0, 255);

    const QImage ordinary =
        terminalCompositedBackgroundImage(source, background, 128, 1.0);
    QCOMPARE(ordinary.pixelColor(0, 0), QColor(255, 0, 0, 128));
    QCOMPARE(ordinary.pixelColor(1, 0), QColor(0, 0, 255, 128));

    const QImage amplified =
        terminalCompositedBackgroundImage(source, background, 128, 1.5);
    QCOMPARE(amplified.pixelColor(0, 0), QColor(255, 0, 0, 192));
    QCOMPARE(amplified.pixelColor(1, 0), QColor(0, 0, 255, 128));

    const QImage transparent =
        terminalCompositedBackgroundImage(source, background, 0, 2.0);
    QCOMPARE(transparent.pixelColor(0, 0), QColor(0, 0, 0, 0));
    QCOMPARE(transparent.pixelColor(1, 0), QColor(0, 0, 0, 0));
    QCOMPARE(source, original);
}

void TerminalBackdropTest::decodesPngIntoPackedRgba()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path =
        directory->filePath(QStringLiteral("straight-alpha.png"));

    QImage source(2, 2, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(1, 2, 3, 4));
    source.setPixelColor(1, 0, QColor(250, 128, 64, 0));
    source.setPixelColor(0, 1, QColor(9, 19, 29, 255));
    source.setPixelColor(1, 1, QColor(101, 151, 201, 127));
    QVERIFY2(source.save(path, "PNG"),
             "Qt could not encode the PNG test fixture.");

    ImageReply reply;
    QObject receiver;
    auto handle = requestTerminalBackgroundImage(imageRequest(path), &receiver,
                                                 captureReply(reply));
    QTRY_VERIFY_WITH_TIMEOUT(reply.result.has_value(), asyncTimeout);
    QCOMPARE(reply.callbackCount, 1);
    QVERIFY2(reply.result->has_value(), qPrintable(replyFailure(reply)));

    const auto &asset = *reply.result->value();
    QCOMPARE(asset.straightRgba.size(), source.size());
    QCOMPARE(asset.straightRgba.format(), QImage::Format_RGBA8888);
    QCOMPARE(asset.straightRgba.devicePixelRatio(), 1.0);
    QCOMPARE(asset.straightRgba.sizeInBytes(),
             static_cast<qsizetype>(source.width()) * source.height() * 4);
    QVERIFY(asset.serial > 0);

    QCOMPARE(packedPixels(asset.straightRgba), expectedPackedBytes(source));
}

void TerminalBackdropTest::decodesJpegIntoPackedRgba()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path =
        directory->filePath(QStringLiteral("opaque-colors.jpg"));
    const QImage source =
        patternedImage(QSize(17, 9)).convertToFormat(QImage::Format_RGB32);
    QVERIFY2(source.save(path, "JPEG", 91),
             "Qt could not encode the JPEG test fixture.");

    QImage decoded;
    QVERIFY2(decoded.load(path, "JPEG"),
             "Qt could not decode its JPEG test fixture.");

    ImageReply reply;
    QObject receiver;
    auto handle = requestTerminalBackgroundImage(imageRequest(path), &receiver,
                                                 captureReply(reply));
    QTRY_VERIFY_WITH_TIMEOUT(reply.result.has_value(), asyncTimeout);
    QCOMPARE(reply.callbackCount, 1);
    QVERIFY2(reply.result->has_value(), qPrintable(replyFailure(reply)));

    const auto &asset = *reply.result->value();
    QCOMPARE(asset.straightRgba.size(), decoded.size());
    QCOMPARE(asset.straightRgba.format(), QImage::Format_RGBA8888);
    QCOMPARE(asset.straightRgba.devicePixelRatio(), 1.0);
    QCOMPARE(asset.straightRgba.sizeInBytes(),
             static_cast<qsizetype>(decoded.width()) * decoded.height() * 4);
    QCOMPARE(packedPixels(asset.straightRgba), expectedPackedBytes(decoded));
}

void TerminalBackdropTest::rejectsUnsupportedAndCorruptImages()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));

    const QString unsupportedPath =
        directory->filePath(QStringLiteral("unsupported.bmp"));
    QImage unsupported(2, 2, QImage::Format_RGB32);
    unsupported.fill(QColor(12, 34, 56));
    QVERIFY2(unsupported.save(unsupportedPath, "BMP"),
             "Qt could not encode the BMP test fixture.");

    ImageReply unsupportedReply;
    QObject unsupportedReceiver;
    auto unsupportedHandle = requestTerminalBackgroundImage(
        imageRequest(unsupportedPath), &unsupportedReceiver,
        captureReply(unsupportedReply));
    QTRY_VERIFY_WITH_TIMEOUT(unsupportedReply.result.has_value(), asyncTimeout);
    QVERIFY(!unsupportedReply.result->has_value());
    QVERIFY2(unsupportedReply.result->error().contains(
                 QStringLiteral("is not PNG or JPEG")),
             qPrintable(unsupportedReply.result->error()));

    const QString corruptPath =
        directory->filePath(QStringLiteral("corrupt.png"));
    QFile corrupt(corruptPath);
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    const QByteArray corruptBytes =
        QByteArray::fromHex("89504e470d0a1a0a0000000d49484452");
    QCOMPARE(corrupt.write(corruptBytes), corruptBytes.size());
    corrupt.close();

    ImageReply corruptReply;
    QObject corruptReceiver;
    auto corruptHandle = requestTerminalBackgroundImage(
        imageRequest(corruptPath), &corruptReceiver,
        captureReply(corruptReply));
    QTRY_VERIFY_WITH_TIMEOUT(corruptReply.result.has_value(), asyncTimeout);
    QVERIFY(!corruptReply.result->has_value());
    QVERIFY2(corruptReply.result->error().contains(
                 QStringLiteral("Could not decode background image")),
             qPrintable(corruptReply.result->error()));

    const QString missingPath =
        directory->filePath(QStringLiteral("missing.png"));
    ImageReply missingReply;
    QObject missingReceiver;
    auto missingHandle = requestTerminalBackgroundImage(
        imageRequest(missingPath), &missingReceiver,
        captureReply(missingReply));
    QTRY_VERIFY_WITH_TIMEOUT(missingReply.result.has_value(), asyncTimeout);
    QVERIFY(!missingReply.result->has_value());
    QVERIFY2(missingReply.result->error().contains(
                 QStringLiteral("Could not open background image")),
             qPrintable(missingReply.result->error()));
}

void TerminalBackdropTest::rejectsSpecialFilesWithoutStarvingPool()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));

    const QString firstPath = directory->filePath(QStringLiteral("first.fifo"));
    const QString secondPath =
        directory->filePath(QStringLiteral("second.fifo"));
    const QByteArray firstNativePath = QFile::encodeName(firstPath);
    const QByteArray secondNativePath = QFile::encodeName(secondPath);
    QVERIFY(::mkfifo(firstNativePath.constData(), 0600) == 0);
    QVERIFY(::mkfifo(secondNativePath.constData(), 0600) == 0);

    const QString imagePath =
        directory->filePath(QStringLiteral("after-special-files.png"));
    const QImage image = solidImage(QColor(21, 41, 61, 81));
    QVERIFY(image.save(imagePath, "PNG"));

    ImageReply firstReply;
    ImageReply secondReply;
    ImageReply imageReply;
    QObject firstReceiver;
    QObject secondReceiver;
    QObject imageReceiver;
    auto firstHandle = requestTerminalBackgroundImage(
        imageRequest(firstPath), &firstReceiver, captureReply(firstReply));
    auto secondHandle = requestTerminalBackgroundImage(
        imageRequest(secondPath), &secondReceiver, captureReply(secondReply));
    auto imageHandle = requestTerminalBackgroundImage(
        imageRequest(imagePath), &imageReceiver, captureReply(imageReply));

    QTRY_VERIFY_WITH_TIMEOUT(firstReply.result.has_value(), asyncTimeout);
    QTRY_VERIFY_WITH_TIMEOUT(secondReply.result.has_value(), asyncTimeout);
    QTRY_VERIFY_WITH_TIMEOUT(imageReply.result.has_value(), asyncTimeout);
    QVERIFY(!firstReply.result->has_value());
    QVERIFY(!secondReply.result->has_value());
    QVERIFY2(firstReply.result->error().contains(
                 QStringLiteral("is not a regular file")),
             qPrintable(firstReply.result->error()));
    QVERIFY2(secondReply.result->error().contains(
                 QStringLiteral("is not a regular file")),
             qPrintable(secondReply.result->error()));
    QVERIFY2(imageReply.result->has_value(),
             qPrintable(replyFailure(imageReply)));
    QCOMPARE(packedPixels(imageReply.result->value()->straightRgba),
             expectedPackedBytes(image));
}

void TerminalBackdropTest::repairedImageBypassesNegativeCache()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path = directory->filePath(QStringLiteral("repaired.png"));
    const QImage image = solidImage(QColor(25, 45, 65, 85));
    const QByteArray validBytes = encodedPng(image);
    QVERIFY(!validBytes.isEmpty());
    const QByteArray corruptBytes(validBytes.size(), '\x7f');

    constexpr qint64 modifiedSeconds = 1'700'000'050;
    constexpr qint64 modifiedNanoseconds = 123'456'789;
    QVERIFY(writeBytes(path, corruptBytes));
    QVERIFY(setModificationTime(path, modifiedSeconds, modifiedNanoseconds));
    const auto corruptStatus = fileStatus(path);
    QVERIFY(corruptStatus.has_value());

    ImageReply corruptReply;
    QObject corruptReceiver;
    auto corruptHandle = requestTerminalBackgroundImage(
        imageRequest(path), &corruptReceiver, captureReply(corruptReply));
    QTRY_VERIFY_WITH_TIMEOUT(corruptReply.result.has_value(), asyncTimeout);
    QVERIFY(!corruptReply.result->has_value());

    QTest::qSleep(2);
    QVERIFY(writeBytes(path, validBytes));
    QVERIFY(setModificationTime(path, modifiedSeconds, modifiedNanoseconds));
    const auto repairedStatus = fileStatus(path);
    QVERIFY(repairedStatus.has_value());
    QCOMPARE(static_cast<quint64>(repairedStatus->st_ino),
             static_cast<quint64>(corruptStatus->st_ino));
    QCOMPARE(static_cast<qint64>(repairedStatus->st_size),
             static_cast<qint64>(corruptStatus->st_size));
    QCOMPARE(static_cast<qint64>(repairedStatus->st_mtim.tv_sec),
             static_cast<qint64>(corruptStatus->st_mtim.tv_sec));
    QCOMPARE(static_cast<qint64>(repairedStatus->st_mtim.tv_nsec),
             static_cast<qint64>(corruptStatus->st_mtim.tv_nsec));
    if (repairedStatus->st_ctim.tv_sec == corruptStatus->st_ctim.tv_sec
        && repairedStatus->st_ctim.tv_nsec == corruptStatus->st_ctim.tv_nsec) {
        QSKIP(
            "The test filesystem does not expose an in-place identity change");
    }

    ImageReply repairedReply;
    QObject repairedReceiver;
    auto repairedHandle = requestTerminalBackgroundImage(
        imageRequest(path), &repairedReceiver, captureReply(repairedReply));
    QTRY_VERIFY_WITH_TIMEOUT(repairedReply.result.has_value(), asyncTimeout);
    QVERIFY2(repairedReply.result->has_value(),
             qPrintable(replyFailure(repairedReply)));
    QCOMPARE(packedPixels(repairedReply.result->value()->straightRgba),
             expectedPackedBytes(image));
}

void TerminalBackdropTest::coalescesIdenticalInflightRequests()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path = directory->filePath(QStringLiteral("coalesced.png"));
    const QImage source = patternedImage(QSize(1024, 1024));
    QVERIFY2(source.save(path, "PNG"),
             "Qt could not encode the PNG test fixture.");

    ImageReply firstReply;
    ImageReply secondReply;
    QObject firstReceiver;
    QObject secondReceiver;
    auto firstHandle = requestTerminalBackgroundImage(
        imageRequest(path), &firstReceiver, captureReply(firstReply));
    auto secondHandle = requestTerminalBackgroundImage(
        imageRequest(path), &secondReceiver, captureReply(secondReply));

    QTRY_VERIFY_WITH_TIMEOUT(firstReply.result.has_value(), asyncTimeout);
    QTRY_VERIFY_WITH_TIMEOUT(secondReply.result.has_value(), asyncTimeout);
    QVERIFY2(firstReply.result->has_value(),
             qPrintable(replyFailure(firstReply)));
    QVERIFY2(secondReply.result->has_value(),
             qPrintable(replyFailure(secondReply)));

    const auto firstAsset = firstReply.result->value();
    const auto secondAsset = secondReply.result->value();
    QCOMPARE(firstReply.callbackCount, 1);
    QCOMPARE(secondReply.callbackCount, 1);
    QCOMPARE(firstAsset->serial, secondAsset->serial);
    QCOMPARE(firstAsset.get(), secondAsset.get());
}

void TerminalBackdropTest::cancellationSuppressesCallback()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path = directory->filePath(QStringLiteral("cancelled.png"));
    QVERIFY2(patternedImage(QSize(512, 512)).save(path, "PNG"),
             "Qt could not encode the PNG test fixture.");

    ImageReply cancelledReply;
    ImageReply survivingReply;
    QObject cancelledReceiver;
    QObject survivingReceiver;
    auto cancelledHandle = requestTerminalBackgroundImage(
        imageRequest(path), &cancelledReceiver, captureReply(cancelledReply));
    cancelledHandle.cancel();
    auto survivingHandle = requestTerminalBackgroundImage(
        imageRequest(path), &survivingReceiver, captureReply(survivingReply));

    QTRY_VERIFY_WITH_TIMEOUT(survivingReply.result.has_value(), asyncTimeout);
    QVERIFY2(survivingReply.result->has_value(),
             qPrintable(replyFailure(survivingReply)));
    QTest::qWait(20);
    QCOMPARE(cancelledReply.callbackCount, 0);
    QVERIFY(!cancelledReply.result.has_value());
}

void TerminalBackdropTest::destroyedReceiverSuppressesCallback()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path =
        directory->filePath(QStringLiteral("destroyed-receiver.png"));
    QVERIFY2(patternedImage(QSize(512, 512)).save(path, "PNG"),
             "Qt could not encode the PNG test fixture.");

    ImageReply destroyedReply;
    ImageReply survivingReply;
    auto destroyedReceiver = std::make_unique<QObject>();
    QObject survivingReceiver;
    auto destroyedHandle = requestTerminalBackgroundImage(
        imageRequest(path), destroyedReceiver.get(),
        captureReply(destroyedReply));
    auto survivingHandle = requestTerminalBackgroundImage(
        imageRequest(path), &survivingReceiver, captureReply(survivingReply));
    destroyedReceiver.reset();

    QTRY_VERIFY_WITH_TIMEOUT(survivingReply.result.has_value(), asyncTimeout);
    QVERIFY2(survivingReply.result->has_value(),
             qPrintable(replyFailure(survivingReply)));
    QTest::qWait(20);
    QCOMPARE(destroyedReply.callbackCount, 0);
    QVERIFY(!destroyedReply.result.has_value());
}

void TerminalBackdropTest::fingerprintsReplacementsAndReusesLiveCache()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path =
        directory->filePath(QStringLiteral("replace-in-place.png"));

    QImage original(1, 1, QImage::Format_RGBA8888);
    original.setPixelColor(0, 0, QColor(7, 17, 27, 37));
    QVERIFY2(original.save(path, "PNG"),
             "Qt could not encode the original PNG fixture.");
    const QFileInfo originalInfo(path);

    ImageReply originalReply;
    QObject originalReceiver;
    auto originalHandle = requestTerminalBackgroundImage(
        imageRequest(path), &originalReceiver, captureReply(originalReply));
    QTRY_VERIFY_WITH_TIMEOUT(originalReply.result.has_value(), asyncTimeout);
    QVERIFY2(originalReply.result->has_value(),
             qPrintable(replyFailure(originalReply)));
    auto originalAsset = originalReply.result->value();

    ImageReply cachedOriginalReply;
    QObject cachedOriginalReceiver;
    auto cachedOriginalHandle = requestTerminalBackgroundImage(
        imageRequest(path), &cachedOriginalReceiver,
        captureReply(cachedOriginalReply));
    QTRY_VERIFY_WITH_TIMEOUT(cachedOriginalReply.result.has_value(),
                             asyncTimeout);
    QVERIFY2(cachedOriginalReply.result->has_value(),
             qPrintable(replyFailure(cachedOriginalReply)));
    auto cachedOriginalAsset = cachedOriginalReply.result->value();
    QCOMPARE(cachedOriginalAsset.get(), originalAsset.get());
    QCOMPARE(cachedOriginalAsset->serial, originalAsset->serial);

    const QImage replacement = patternedImage(QSize(13, 7));
    QVERIFY2(replacement.save(path, "PNG"),
             "Qt could not encode the replacement PNG fixture.");
    QFile replacementFile(path);
    QVERIFY(replacementFile.open(QIODevice::ReadWrite));
    QVERIFY(replacementFile.setFileTime(originalInfo.lastModified().addSecs(5),
                                        QFileDevice::FileModificationTime));
    replacementFile.close();
    const QFileInfo replacementInfo(path);
    QVERIFY(replacementInfo.size() != originalInfo.size()
            || replacementInfo.lastModified() != originalInfo.lastModified());

    ImageReply replacementReply;
    QObject replacementReceiver;
    auto replacementHandle =
        requestTerminalBackgroundImage(imageRequest(path), &replacementReceiver,
                                       captureReply(replacementReply));
    QTRY_VERIFY_WITH_TIMEOUT(replacementReply.result.has_value(), asyncTimeout);
    QVERIFY2(replacementReply.result->has_value(),
             qPrintable(replyFailure(replacementReply)));
    auto replacementAsset = replacementReply.result->value();
    QVERIFY(replacementAsset->serial != originalAsset->serial);
    QCOMPARE(replacementAsset->straightRgba.size(), replacement.size());
    const QByteArray expectedReplacement = expectedPackedBytes(replacement);
    QCOMPARE(packedPixels(replacementAsset->straightRgba), expectedReplacement);

    ImageReply cachedReplacementReply;
    QObject cachedReplacementReceiver;
    auto cachedReplacementHandle = requestTerminalBackgroundImage(
        imageRequest(path), &cachedReplacementReceiver,
        captureReply(cachedReplacementReply));
    QTRY_VERIFY_WITH_TIMEOUT(cachedReplacementReply.result.has_value(),
                             asyncTimeout);
    QVERIFY2(cachedReplacementReply.result->has_value(),
             qPrintable(replyFailure(cachedReplacementReply)));
    auto cachedReplacementAsset = cachedReplacementReply.result->value();
    QCOMPARE(cachedReplacementAsset.get(), replacementAsset.get());
    const quint64 replacementSerial = replacementAsset->serial;

    replacementAsset.reset();
    cachedReplacementAsset.reset();
    replacementReply.result.reset();
    cachedReplacementReply.result.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    ImageReply redecodedReply;
    QObject redecodedReceiver;
    auto redecodedHandle = requestTerminalBackgroundImage(
        imageRequest(path), &redecodedReceiver, captureReply(redecodedReply));
    QTRY_VERIFY_WITH_TIMEOUT(redecodedReply.result.has_value(), asyncTimeout);
    QVERIFY2(redecodedReply.result->has_value(),
             qPrintable(replyFailure(redecodedReply)));
    const auto redecodedAsset = redecodedReply.result->value();
    QVERIFY(redecodedAsset->serial != replacementSerial);
    QCOMPARE(packedPixels(redecodedAsset->straightRgba), expectedReplacement);
}

void TerminalBackdropTest::detectsSameMillisecondInPlaceRewrite()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path =
        directory->filePath(QStringLiteral("submillisecond-rewrite.png"));
    const QImage original = solidImage(QColor(9, 19, 29, 39));
    const QImage replacement = solidImage(QColor(109, 119, 129, 139));
    QByteArray originalBytes = encodedPng(original);
    QByteArray replacementBytes = encodedPng(replacement);
    QVERIFY(!originalBytes.isEmpty());
    QVERIFY(!replacementBytes.isEmpty());
    equalizeByteLengths(originalBytes, replacementBytes);
    QCOMPARE(originalBytes.size(), replacementBytes.size());
    QVERIFY(!QImage::fromData(originalBytes, "PNG").isNull());
    QVERIFY(!QImage::fromData(replacementBytes, "PNG").isNull());

    constexpr qint64 modifiedSeconds = 1'700'000'000;
    constexpr qint64 originalNanoseconds = 456'100'100;
    constexpr qint64 replacementNanoseconds = 456'100'900;
    QVERIFY(writeBytes(path, originalBytes));
    QVERIFY(setModificationTime(path, modifiedSeconds, originalNanoseconds));
    const auto originalStatus = fileStatus(path);
    QVERIFY(originalStatus.has_value());
    if (originalStatus->st_mtim.tv_nsec != originalNanoseconds) {
        QSKIP("The test filesystem does not preserve nanosecond mtimes");
    }

    ImageReply originalReply;
    QObject originalReceiver;
    auto originalHandle = requestTerminalBackgroundImage(
        imageRequest(path), &originalReceiver, captureReply(originalReply));
    QTRY_VERIFY_WITH_TIMEOUT(originalReply.result.has_value(), asyncTimeout);
    QVERIFY2(originalReply.result->has_value(),
             qPrintable(replyFailure(originalReply)));
    const auto originalAsset = originalReply.result->value();
    QCOMPARE(packedPixels(originalAsset->straightRgba),
             expectedPackedBytes(original));

    // Preserve the legacy path/size/millisecond fingerprint while changing
    // both the bytes and the descriptor's nanosecond identity.
    QVERIFY(writeBytes(path, replacementBytes));
    QVERIFY(setModificationTime(path, modifiedSeconds, replacementNanoseconds));
    const auto replacementStatus = fileStatus(path);
    QVERIFY(replacementStatus.has_value());
    if (replacementStatus->st_mtim.tv_nsec != replacementNanoseconds) {
        QSKIP("The test filesystem does not preserve nanosecond mtimes");
    }
    QCOMPARE(static_cast<quint64>(replacementStatus->st_ino),
             static_cast<quint64>(originalStatus->st_ino));
    QCOMPARE(static_cast<qint64>(replacementStatus->st_size),
             static_cast<qint64>(originalStatus->st_size));
    QCOMPARE(static_cast<qint64>(replacementStatus->st_mtim.tv_sec),
             static_cast<qint64>(originalStatus->st_mtim.tv_sec));
    QCOMPARE(replacementStatus->st_mtim.tv_nsec / 1'000'000,
             originalStatus->st_mtim.tv_nsec / 1'000'000);
    QVERIFY(replacementStatus->st_mtim.tv_nsec
            != originalStatus->st_mtim.tv_nsec);

    ImageReply replacementReply;
    QObject replacementReceiver;
    auto replacementHandle =
        requestTerminalBackgroundImage(imageRequest(path), &replacementReceiver,
                                       captureReply(replacementReply));
    QTRY_VERIFY_WITH_TIMEOUT(replacementReply.result.has_value(), asyncTimeout);
    QVERIFY2(replacementReply.result->has_value(),
             qPrintable(replyFailure(replacementReply)));
    const auto replacementAsset = replacementReply.result->value();
    QVERIFY(replacementAsset->serial != originalAsset->serial);
    QCOMPARE(packedPixels(replacementAsset->straightRgba),
             expectedPackedBytes(replacement));
}

void TerminalBackdropTest::detectsAtomicReplacementWithIdenticalMetadata()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path =
        directory->filePath(QStringLiteral("atomic-target.png"));
    const QString stagedPath =
        directory->filePath(QStringLiteral("atomic-replacement.png"));
    const QImage original = solidImage(QColor(17, 27, 37, 47));
    const QImage replacement = solidImage(QColor(117, 127, 137, 147));
    QByteArray originalBytes = encodedPng(original);
    QByteArray replacementBytes = encodedPng(replacement);
    QVERIFY(!originalBytes.isEmpty());
    QVERIFY(!replacementBytes.isEmpty());
    equalizeByteLengths(originalBytes, replacementBytes);
    QVERIFY(writeBytes(path, originalBytes));
    QVERIFY(writeBytes(stagedPath, replacementBytes));

    constexpr qint64 modifiedSeconds = 1'700'000'100;
    constexpr qint64 modifiedNanoseconds = 234'567'890;
    QVERIFY(setModificationTime(path, modifiedSeconds, modifiedNanoseconds));
    QVERIFY(
        setModificationTime(stagedPath, modifiedSeconds, modifiedNanoseconds));
    const auto originalStatus = fileStatus(path);
    const auto stagedStatus = fileStatus(stagedPath);
    QVERIFY(originalStatus.has_value());
    QVERIFY(stagedStatus.has_value());
    QVERIFY(originalStatus->st_ino != stagedStatus->st_ino);
    QCOMPARE(static_cast<qint64>(originalStatus->st_size),
             static_cast<qint64>(stagedStatus->st_size));
    QCOMPARE(static_cast<qint64>(originalStatus->st_mtim.tv_sec),
             static_cast<qint64>(stagedStatus->st_mtim.tv_sec));
    QCOMPARE(static_cast<qint64>(originalStatus->st_mtim.tv_nsec),
             static_cast<qint64>(stagedStatus->st_mtim.tv_nsec));

    ImageReply originalReply;
    QObject originalReceiver;
    auto originalHandle = requestTerminalBackgroundImage(
        imageRequest(path), &originalReceiver, captureReply(originalReply));
    QTRY_VERIFY_WITH_TIMEOUT(originalReply.result.has_value(), asyncTimeout);
    QVERIFY2(originalReply.result->has_value(),
             qPrintable(replyFailure(originalReply)));
    const auto originalAsset = originalReply.result->value();

    QVERIFY(replaceAtomically(stagedPath, path));
    const auto currentStatus = fileStatus(path);
    QVERIFY(currentStatus.has_value());
    QCOMPARE(static_cast<quint64>(currentStatus->st_ino),
             static_cast<quint64>(stagedStatus->st_ino));
    QCOMPARE(static_cast<qint64>(currentStatus->st_size),
             static_cast<qint64>(originalStatus->st_size));
    QCOMPARE(static_cast<qint64>(currentStatus->st_mtim.tv_sec),
             static_cast<qint64>(originalStatus->st_mtim.tv_sec));
    QCOMPARE(currentStatus->st_mtim.tv_nsec / 1'000'000,
             originalStatus->st_mtim.tv_nsec / 1'000'000);

    ImageReply replacementReply;
    QObject replacementReceiver;
    auto replacementHandle =
        requestTerminalBackgroundImage(imageRequest(path), &replacementReceiver,
                                       captureReply(replacementReply));
    QTRY_VERIFY_WITH_TIMEOUT(replacementReply.result.has_value(), asyncTimeout);
    QVERIFY2(replacementReply.result->has_value(),
             qPrintable(replyFailure(replacementReply)));
    const auto replacementAsset = replacementReply.result->value();
    QVERIFY(replacementAsset->serial != originalAsset->serial);
    QCOMPARE(packedPixels(replacementAsset->straightRgba),
             expectedPackedBytes(replacement));
}

void TerminalBackdropTest::decodesOpenedFileAcrossAtomicReplacement()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path =
        directory->filePath(QStringLiteral("open-race-target.png"));
    const QString stagedPath =
        directory->filePath(QStringLiteral("open-race-replacement.png"));
    const QImage original = solidImage(QColor(23, 43, 63, 83));
    const QImage replacement = solidImage(QColor(123, 143, 163, 183));
    QByteArray originalBytes = encodedPng(original);
    QByteArray replacementBytes = encodedPng(replacement);
    QVERIFY(!originalBytes.isEmpty());
    QVERIFY(!replacementBytes.isEmpty());
    equalizeByteLengths(originalBytes, replacementBytes);
    QVERIFY(writeBytes(path, originalBytes));
    QVERIFY(writeBytes(stagedPath, replacementBytes));
    constexpr qint64 modifiedSeconds = 1'700'000'200;
    constexpr qint64 modifiedNanoseconds = 345'678'901;
    QVERIFY(setModificationTime(path, modifiedSeconds, modifiedNanoseconds));
    QVERIFY(
        setModificationTime(stagedPath, modifiedSeconds, modifiedNanoseconds));

    struct DecodeGate {
        QSemaphore reached;
        QSemaphore resume;
    };
    const auto gate = std::make_shared<DecodeGate>();
    bool resumed = false;
    const auto resumeGuard = qScopeGuard([gate, &resumed] {
        if (!resumed) gate->resume.release();
    });

    ImageReply originalReply;
    QObject originalReceiver;
    auto originalHandle = requestTerminalBackgroundImageForTest(
        imageRequest(path), &originalReceiver, captureReply(originalReply),
        [gate] {
            gate->reached.release();
            gate->resume.acquire();
        });
    QVERIFY(gate->reached.tryAcquire(1, asyncTimeout));
    QVERIFY(replaceAtomically(stagedPath, path));

    // The replacement has a different descriptor identity, so it neither
    // joins nor waits for the paused old-inode leader.
    ImageReply replacementReply;
    QObject replacementReceiver;
    auto replacementHandle =
        requestTerminalBackgroundImage(imageRequest(path), &replacementReceiver,
                                       captureReply(replacementReply));
    QTRY_VERIFY_WITH_TIMEOUT(replacementReply.result.has_value(), asyncTimeout);
    QVERIFY2(replacementReply.result->has_value(),
             qPrintable(replyFailure(replacementReply)));
    const auto replacementAsset = replacementReply.result->value();
    QCOMPARE(packedPixels(replacementAsset->straightRgba),
             expectedPackedBytes(replacement));
    QVERIFY(!originalReply.result.has_value());

    gate->resume.release();
    resumed = true;
    QTRY_VERIFY_WITH_TIMEOUT(originalReply.result.has_value(), asyncTimeout);
    QVERIFY2(originalReply.result->has_value(),
             qPrintable(replyFailure(originalReply)));
    const auto originalAsset = originalReply.result->value();
    QCOMPARE(packedPixels(originalAsset->straightRgba),
             expectedPackedBytes(original));
    QVERIFY(originalAsset->serial != replacementAsset->serial);
}

void TerminalBackdropTest::retriesInPlaceRewriteAfterOpen()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path =
        directory->filePath(QStringLiteral("open-in-place-race.png"));
    const QImage original = solidImage(QColor(31, 51, 71, 91));
    const QImage replacement = solidImage(QColor(131, 151, 171, 191));
    QByteArray originalBytes = encodedPng(original);
    QByteArray replacementBytes = encodedPng(replacement);
    QVERIFY(!originalBytes.isEmpty());
    QVERIFY(!replacementBytes.isEmpty());
    equalizeByteLengths(originalBytes, replacementBytes);
    QVERIFY(writeBytes(path, originalBytes));
    constexpr qint64 modifiedSeconds = 1'700'000'300;
    constexpr qint64 modifiedNanoseconds = 456'789'012;
    QVERIFY(setModificationTime(path, modifiedSeconds, modifiedNanoseconds));
    const auto originalStatus = fileStatus(path);
    QVERIFY(originalStatus.has_value());

    struct DecodeGate {
        QSemaphore reached;
        QSemaphore resume;
    };
    const auto gate = std::make_shared<DecodeGate>();
    bool resumed = false;
    const auto resumeGuard = qScopeGuard([gate, &resumed] {
        if (!resumed) gate->resume.release();
    });

    ImageReply replacementReply;
    QObject replacementReceiver;
    auto replacementHandle = requestTerminalBackgroundImageForTest(
        imageRequest(path), &replacementReceiver,
        captureReply(replacementReply), [gate] {
            gate->reached.release();
            gate->resume.acquire();
        });
    QVERIFY(gate->reached.tryAcquire(1, asyncTimeout));

    QTest::qSleep(2);
    QVERIFY(writeBytes(path, replacementBytes));
    QVERIFY(setModificationTime(path, modifiedSeconds, modifiedNanoseconds));
    const auto changedStatus = fileStatus(path);
    QVERIFY(changedStatus.has_value());
    QCOMPARE(static_cast<quint64>(changedStatus->st_ino),
             static_cast<quint64>(originalStatus->st_ino));
    QCOMPARE(static_cast<qint64>(changedStatus->st_size),
             static_cast<qint64>(originalStatus->st_size));
    QCOMPARE(static_cast<qint64>(changedStatus->st_mtim.tv_sec),
             static_cast<qint64>(originalStatus->st_mtim.tv_sec));
    QCOMPARE(static_cast<qint64>(changedStatus->st_mtim.tv_nsec),
             static_cast<qint64>(originalStatus->st_mtim.tv_nsec));
    if (changedStatus->st_ctim.tv_sec == originalStatus->st_ctim.tv_sec
        && changedStatus->st_ctim.tv_nsec == originalStatus->st_ctim.tv_nsec) {
        gate->resume.release();
        resumed = true;
        QSKIP(
            "The test filesystem does not expose an in-place identity change");
    }

    gate->resume.release();
    resumed = true;
    QTRY_VERIFY_WITH_TIMEOUT(replacementReply.result.has_value(), asyncTimeout);
    QVERIFY2(replacementReply.result->has_value(),
             qPrintable(replyFailure(replacementReply)));
    const auto replacementAsset = replacementReply.result->value();
    QCOMPARE(packedPixels(replacementAsset->straightRgba),
             expectedPackedBytes(replacement));

    // The coherent retry is cached under the post-mutation identity rather
    // than publishing the first attempt under stale metadata.
    ImageReply cachedReply;
    QObject cachedReceiver;
    auto cachedHandle = requestTerminalBackgroundImage(
        imageRequest(path), &cachedReceiver, captureReply(cachedReply));
    QTRY_VERIFY_WITH_TIMEOUT(cachedReply.result.has_value(), asyncTimeout);
    QVERIFY2(cachedReply.result->has_value(),
             qPrintable(replyFailure(cachedReply)));
    QCOMPARE(cachedReply.result->value().get(), replacementAsset.get());
}

void TerminalBackdropTest::deliversCallbacksOnApplicationThread()
{
    const auto directory = makeTestDirectory();
    QVERIFY2(directory->isValid(), qPrintable(directory->errorString()));
    const QString path =
        directory->filePath(QStringLiteral("callback-thread.png"));
    QImage source(2, 2, QImage::Format_RGB32);
    source.fill(QColor(23, 45, 67));
    QVERIFY2(source.save(path, "PNG"),
             "Qt could not encode the PNG test fixture.");

    ImageReply reply;
    QObject receiver;
    bool requestReturned = false;
    QThread *const requestingThread = QThread::currentThread();
    auto handle = requestTerminalBackgroundImage(
        imageRequest(path), &receiver, captureReply(reply, &requestReturned));
    requestReturned = true;

    QTRY_VERIFY_WITH_TIMEOUT(reply.result.has_value(), asyncTimeout);
    QVERIFY2(reply.result->has_value(), qPrintable(replyFailure(reply)));
    QCOMPARE(reply.callbackCount, 1);
    QVERIFY(reply.callbackWasQueued);
    QCOMPARE(reply.callbackThread, QCoreApplication::instance()->thread());
    QCOMPARE(reply.callbackThread, requestingThread);
}

QTEST_GUILESS_MAIN(TerminalBackdropTest)

#include "test_terminal_backdrop.moc"
