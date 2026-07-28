#include "terminal_backdrop.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <memory>
#include <optional>
#include <utility>

namespace {

constexpr auto asyncTimeout = 3'000;

struct ImageReply {
    std::optional<TerminalBackgroundImageResult> result;
    QThread *callbackThread = nullptr;
    int callbackCount = 0;
    bool callbackWasQueued = false;
};

struct PlaneBytes {
    QByteArray rgb;
    QByteArray alpha;
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

PlaneBytes expectedPlaneBytes(const QImage &source)
{
    const QImage straight = source.convertToFormat(QImage::Format_RGBA8888);
    PlaneBytes result;
    const qsizetype pixelCount =
        static_cast<qsizetype>(straight.width()) * straight.height();
    result.rgb.reserve(pixelCount * 4);
    result.alpha.reserve(pixelCount * 4);

    for (int y = 0; y < straight.height(); ++y) {
        const uchar *line = straight.constScanLine(y);
        for (int x = 0; x < straight.width(); ++x) {
            const qsizetype offset = static_cast<qsizetype>(x) * 4;
            result.rgb.append(static_cast<char>(line[offset]));
            result.rgb.append(static_cast<char>(line[offset + 1]));
            result.rgb.append(static_cast<char>(line[offset + 2]));
            result.rgb.append(static_cast<char>(255));

            const char alpha = static_cast<char>(line[offset + 3]);
            result.alpha.append(alpha);
            result.alpha.append(alpha);
            result.alpha.append(alpha);
            result.alpha.append(static_cast<char>(255));
        }
    }
    return result;
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

} // namespace

class TerminalBackdropTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void placesEveryFitInPhysicalSpace();
    void anchorsEveryPosition_data();
    void anchorsEveryPosition();
    void composesGhosttyOpacityExactly();
    void decodesPngIntoStraightPlanes();
    void decodesJpegIntoStraightPlanes();
    void rejectsUnsupportedAndCorruptImages();
    void coalescesIdenticalInflightRequests();
    void cancellationSuppressesCallback();
    void destroyedReceiverSuppressesCallback();
    void fingerprintsReplacementsAndReusesLiveCache();
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
}

void TerminalBackdropTest::decodesPngIntoStraightPlanes()
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
    QCOMPARE(asset.straightRgbPlane.size(), source.size());
    QCOMPARE(asset.alphaPlane.size(), source.size());
    QCOMPARE(asset.straightRgbPlane.format(), QImage::Format_RGBX8888);
    QCOMPARE(asset.alphaPlane.format(), QImage::Format_RGBX8888);
    QCOMPARE(asset.straightRgbPlane.devicePixelRatio(), 1.0);
    QCOMPARE(asset.alphaPlane.devicePixelRatio(), 1.0);
    QVERIFY(asset.serial > 0);

    const PlaneBytes expected = expectedPlaneBytes(source);
    QCOMPARE(packedPixels(asset.straightRgbPlane), expected.rgb);
    QCOMPARE(packedPixels(asset.alphaPlane), expected.alpha);
}

void TerminalBackdropTest::decodesJpegIntoStraightPlanes()
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
    QCOMPARE(asset.straightRgbPlane.size(), decoded.size());
    QCOMPARE(asset.alphaPlane.size(), decoded.size());
    QCOMPARE(asset.straightRgbPlane.format(), QImage::Format_RGBX8888);
    QCOMPARE(asset.alphaPlane.format(), QImage::Format_RGBX8888);

    const PlaneBytes expected = expectedPlaneBytes(decoded);
    QCOMPARE(packedPixels(asset.straightRgbPlane), expected.rgb);
    QCOMPARE(packedPixels(asset.alphaPlane), expected.alpha);
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
    QCOMPARE(replacementAsset->straightRgbPlane.size(), replacement.size());
    const PlaneBytes expectedReplacement = expectedPlaneBytes(replacement);
    QCOMPARE(packedPixels(replacementAsset->straightRgbPlane),
             expectedReplacement.rgb);
    QCOMPARE(packedPixels(replacementAsset->alphaPlane),
             expectedReplacement.alpha);

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
    QCOMPARE(packedPixels(redecodedAsset->straightRgbPlane),
             expectedReplacement.rgb);
    QCOMPARE(packedPixels(redecodedAsset->alphaPlane),
             expectedReplacement.alpha);
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
