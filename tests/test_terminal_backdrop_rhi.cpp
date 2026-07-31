#include "terminal_backdrop_qsg.h"

#include <QFile>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <QtMath>
#include <rhi/qshader.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {

QSGRendererInterface::GraphicsApi requestedGraphicsApi =
    QSGRendererInterface::OpenGL;

struct TestImage {
    QImage rgba{QSize(2, 1), QImage::Format_RGBA8888};
};

void setPixel(TestImage &image, int x, int y, QColor color, int alpha)
{
    color.setAlpha(alpha);
    image.rgba.setPixelColor(x, y, color);
}

bool near(int actual, int expected, int tolerance = 8)
{
    return std::abs(actual - expected) <= tolerance;
}

class BackdropItem final : public QQuickItem {
public:
    BackdropItem()
    {
        setFlag(QQuickItem::ItemHasContents);
    }

    void setImage(TestImage image, quint64 serial)
    {
        image_ = std::move(image);
        serial_ = serial;
        update();
    }

    void clearImage()
    {
        image_.rgba = {};
        serial_ = 0;
        update();
    }

    void synchronizeTelemetry() { update(); }

    void setComposition(QColor background, double imageOpacity, bool repeat,
                        QRectF destination)
    {
        background_ = std::move(background);
        imageOpacity_ = imageOpacity;
        repeat_ = repeat;
        destination_ = destination;
        update();
    }

    [[nodiscard]] quint64 textureGeneration() const noexcept
    {
        return textureGeneration_.load(std::memory_order_acquire);
    }

    [[nodiscard]] quint64 textureUploadCount() const noexcept
    {
        return textureUploadCount_.load(std::memory_order_acquire);
    }

    [[nodiscard]] quint64 textureCount() const noexcept
    {
        return textureCount_.load(std::memory_order_acquire);
    }

    [[nodiscard]] quint64 textureBytes() const noexcept
    {
        return textureBytes_.load(std::memory_order_acquire);
    }

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *) override
    {
        auto *node = oldNode != nullptr
            ? static_cast<TerminalBackdropQsgNode *>(oldNode)
            : new TerminalBackdropQsgNode;
        (void)node->update(window(), image_.rgba, serial_, boundingRect(),
                           background_, imageOpacity_, repeat_, destination_);
        textureGeneration_.store(node->textureGeneration(),
                                 std::memory_order_release);
        textureUploadCount_.store(node->textureUploadCount(),
                                  std::memory_order_release);
        textureCount_.store(node->textureCount(), std::memory_order_release);
        textureBytes_.store(node->textureBytes(), std::memory_order_release);
        return node;
    }

private:
    TestImage image_;
    quint64 serial_ = 1;
    QColor background_ = Qt::black;
    double imageOpacity_ = 1.0;
    bool repeat_ = false;
    QRectF destination_{0.0, 0.0, 2.0, 1.0};
    std::atomic<quint64> textureGeneration_{0};
    std::atomic<quint64> textureUploadCount_{0};
    std::atomic<quint64> textureCount_{0};
    std::atomic<quint64> textureBytes_{0};
};

QImage render(BackdropItem &item, QQuickWindow &window, QSize size)
{
    window.setColor(Qt::transparent);
    window.resize(size);
    item.setParentItem(window.contentItem());
    item.setSize(QSizeF(size));
    window.show();
    if (!QTest::qWaitForWindowExposed(&window, 5'000)) return {};

    QImage frame;
    for (int attempt = 0; attempt < 50 && frame.isNull(); ++attempt) {
        frame = window.grabWindow();
        if (frame.isNull()) QTest::qWait(20);
    }
    return frame;
}

} // namespace

class TerminalBackdropRhiTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void embedsShaderResources();
    void samplesStraightAlphaBeforePremultiplication();
    void preservesPackedTextureOrientation();
    void repeatsWithAHardModuloSeam();
    void composesRelativeAndGlobalOpacity();
    void optionChangesDoNotUploadTexturesAgain();
};

void TerminalBackdropRhiTest::initTestCase()
{
    QCOMPARE(QQuickWindow::graphicsApi(), requestedGraphicsApi);
}

void TerminalBackdropRhiTest::embedsShaderResources()
{
    QVERIFY(QFile::exists(QStringLiteral(
        ":/ghostty-qt/shaders/terminal_backdrop.vert.qsb")));
    QVERIFY(QFile::exists(QStringLiteral(
        ":/ghostty-qt/shaders/terminal_backdrop.frag.qsb")));

    QFile fragment(
        QStringLiteral(":/ghostty-qt/shaders/terminal_backdrop.frag.qsb"));
    QVERIFY2(fragment.open(QIODevice::ReadOnly),
             qPrintable(fragment.errorString()));
    const QShader shader = QShader::fromSerialized(fragment.readAll());
    QVERIFY(shader.isValid());
    QCOMPARE(shader.stage(), QShader::FragmentStage);
    const auto blocks = shader.description().uniformBlocks();
    QCOMPARE(blocks.size(), 1);
    QCOMPARE(blocks.constFirst().binding, 0);
    QCOMPARE(blocks.constFirst().size, 112);
    const auto samplers = shader.description().combinedImageSamplers();
    QCOMPARE(samplers.size(), 1);
    QCOMPARE(samplers.constFirst().name, QByteArray("straightRgba"));
    QCOMPARE(samplers.constFirst().binding, 1);
}

void TerminalBackdropRhiTest::samplesStraightAlphaBeforePremultiplication()
{
    TestImage image;
    setPixel(image, 0, 0, Qt::red, 255);
    setPixel(image, 1, 0, Qt::green, 0);

    QQuickWindow window;
    BackdropItem item;
    item.setImage(std::move(image), 1);
    item.setComposition(Qt::black, 1.0, false,
                        QRectF(0.0, 0.0, 4.0, 1.0));
    const QImage frame = render(item, window, QSize(4, 1));
    if (frame.isNull()) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    if (!QSGRendererInterface::isApiRhiBased(
            window.rendererInterface()->graphicsApi())) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    QCOMPARE(window.rendererInterface()->graphicsApi(), requestedGraphicsApi);

    // At this sample, straight filtering preserves green from the transparent
    // texel before the interpolated color is premultiplied. Premultiplied
    // texture filtering would produce no green at all.
    const QColor sample = frame.pixelColor(2, 0);
    QVERIFY2(sample.green() > 20,
             qPrintable(QStringLiteral("unexpected sample %1,%2,%3,%4")
                            .arg(sample.red())
                            .arg(sample.green())
                            .arg(sample.blue())
                            .arg(sample.alpha())));
    QVERIFY(sample.red() > 0);
    QCOMPARE(sample.alpha(), 255);
}

void TerminalBackdropRhiTest::preservesPackedTextureOrientation()
{
    TestImage image;
    image.rgba = QImage(QSize(2, 2), QImage::Format_RGBA8888);
    setPixel(image, 0, 0, Qt::red, 255);
    setPixel(image, 1, 0, Qt::green, 255);
    setPixel(image, 0, 1, Qt::blue, 255);
    setPixel(image, 1, 1, Qt::yellow, 255);

    QQuickWindow window;
    BackdropItem item;
    item.setImage(std::move(image), 2);
    item.setComposition(Qt::black, 1.0, false, QRectF(0.0, 0.0, 2.0, 2.0));
    const QImage frame = render(item, window, QSize(2, 2));
    if (frame.isNull()) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    if (!QSGRendererInterface::isApiRhiBased(
            window.rendererInterface()->graphicsApi())) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    QCOMPARE(window.rendererInterface()->graphicsApi(), requestedGraphicsApi);
    const qreal dpr = window.devicePixelRatio();
    const int left = std::clamp(qFloor(0.5 * dpr), 0, frame.width() - 1);
    const int right = std::clamp(qFloor(1.5 * dpr), 0, frame.width() - 1);
    const int top = std::clamp(qFloor(0.5 * dpr), 0, frame.height() - 1);
    const int bottom = std::clamp(qFloor(1.5 * dpr), 0, frame.height() - 1);
    QVERIFY(right > left);
    QVERIFY(bottom > top);
    const auto mostly = [](const QColor &actual, const QColor &expected) {
        return near(actual.red(), expected.red(), 16)
            && near(actual.green(), expected.green(), 16)
            && near(actual.blue(), expected.blue(), 16);
    };
    QVERIFY(mostly(frame.pixelColor(left, top), Qt::red));
    QVERIFY(mostly(frame.pixelColor(right, top), Qt::green));
    QVERIFY(mostly(frame.pixelColor(left, bottom), Qt::blue));
    QVERIFY(mostly(frame.pixelColor(right, bottom), Qt::yellow));
}

void TerminalBackdropRhiTest::repeatsWithAHardModuloSeam()
{
    TestImage image;
    setPixel(image, 0, 0, Qt::red, 255);
    setPixel(image, 1, 0, Qt::blue, 255);

    QQuickWindow window;
    BackdropItem item;
    item.setImage(std::move(image), 3);
    item.setComposition(Qt::black, 1.0, true,
                        QRectF(0.0, 0.0, 8.0, 1.0));
    const QImage frame = render(item, window, QSize(16, 1));
    if (frame.isNull()) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    if (!QSGRendererInterface::isApiRhiBased(
            window.rendererInterface()->graphicsApi())) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    QCOMPARE(window.rendererInterface()->graphicsApi(), requestedGraphicsApi);

    const int tileWidth =
        qRound(8.0 * window.devicePixelRatio());
    QVERIFY(tileWidth > 1);
    QVERIFY(frame.width() >= 2 * tileWidth);
    const QColor beforeSeam = frame.pixelColor(tileWidth - 1, 0);
    const QColor afterSeam = frame.pixelColor(tileWidth, 0);
    QVERIFY2(beforeSeam.blue() > afterSeam.blue() + 100,
             qPrintable(QStringLiteral("before=%1,%2,%3 after=%4,%5,%6")
                            .arg(beforeSeam.red())
                            .arg(beforeSeam.green())
                            .arg(beforeSeam.blue())
                            .arg(afterSeam.red())
                            .arg(afterSeam.green())
                            .arg(afterSeam.blue())));
    QVERIFY(afterSeam.red() > beforeSeam.red() + 100);

    // The next tile repeats the first one exactly. A sampler-level repeat
    // would be periodic too, but would fail the discontinuity above by
    // interpolating the last and first texels across the boundary.
    for (int x = 0; x < tileWidth; ++x) {
        const QColor firstTile = frame.pixelColor(x, 0);
        const QColor secondTile = frame.pixelColor(x + tileWidth, 0);
        QVERIFY(near(secondTile.red(), firstTile.red()));
        QVERIFY(near(secondTile.green(), firstTile.green()));
        QVERIFY(near(secondTile.blue(), firstTile.blue()));
    }
}

void TerminalBackdropRhiTest::composesRelativeAndGlobalOpacity()
{
    TestImage image;
    setPixel(image, 0, 0, Qt::red, 255);
    setPixel(image, 1, 0, Qt::red, 255);

    QQuickWindow window;
    BackdropItem item;
    item.setImage(std::move(image), 4);
    item.setComposition(QColor(0, 0, 255, 128), 1.5, false,
                        QRectF(0.0, 0.0, 4.0, 2.0));
    QImage frame = render(item, window, QSize(4, 2));
    if (frame.isNull()) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    if (!QSGRendererInterface::isApiRhiBased(
            window.rendererInterface()->graphicsApi())) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    QCOMPARE(window.rendererInterface()->graphicsApi(), requestedGraphicsApi);

    const QPoint samplePoint(
        std::min(frame.width() - 1,
                 qRound(2.0 * window.devicePixelRatio())),
        0);
    QColor sample = frame.pixelColor(samplePoint);
    QVERIFY2(near(sample.red(), 255),
             qPrintable(QStringLiteral("amplified=%1,%2,%3,%4")
                            .arg(sample.red())
                            .arg(sample.green())
                            .arg(sample.blue())
                            .arg(sample.alpha())));
    QVERIFY(sample.green() < 8);
    QVERIFY(sample.blue() < 8);
    QVERIFY(near(sample.alpha(), 192));

    item.setOpacity(0.5);
    frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    sample = frame.pixelColor(samplePoint);
    QVERIFY(near(sample.red(), 255));
    QVERIFY(near(sample.alpha(), 96));
    item.setOpacity(1.0);

    item.setComposition(QColor(0, 0, 255, 128), 0.0, false,
                        QRectF(0.0, 0.0, 4.0, 2.0));
    frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    sample = frame.pixelColor(samplePoint);
    QVERIFY(sample.red() < 8);
    QVERIFY(sample.green() < 8);
    QVERIFY(near(sample.blue(), 255));
    QVERIFY(near(sample.alpha(), 128));

    item.setComposition(QColor(0, 0, 255, 0), 2.0, false,
                        QRectF(0.0, 0.0, 4.0, 2.0));
    frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    sample = frame.pixelColor(samplePoint);
    QVERIFY(sample.alpha() < 8);
}

void TerminalBackdropRhiTest::optionChangesDoNotUploadTexturesAgain()
{
    TestImage image;
    setPixel(image, 0, 0, Qt::red, 255);
    setPixel(image, 1, 0, Qt::blue, 255);

    QQuickWindow window;
    BackdropItem item;
    item.setImage(image, 5);
    item.setComposition(QColor(0, 0, 0, 128), 1.0, false,
                        QRectF(0.0, 0.0, 2.0, 1.0));
    if (render(item, window, QSize(6, 1)).isNull()) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    if (!QSGRendererInterface::isApiRhiBased(
            window.rendererInterface()->graphicsApi())) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    QCOMPARE(window.rendererInterface()->graphicsApi(), requestedGraphicsApi);
    QTRY_COMPARE_WITH_TIMEOUT(item.textureGeneration(), quint64{1}, 5'000);
    QCOMPARE(item.textureCount(), quint64{1});
    QCOMPARE(item.textureBytes(), quint64{8});
    item.synchronizeTelemetry();
    QVERIFY(!window.grabWindow().isNull());
    QTRY_COMPARE_WITH_TIMEOUT(item.textureUploadCount(), quint64{1}, 5'000);

    item.setComposition(QColor(10, 20, 30, 192), 1.5, true,
                        QRectF(2.0, 0.0, 2.0, 1.0));
    QVERIFY(!window.grabWindow().isNull());
    QCOMPARE(item.textureGeneration(), quint64{1});
    QCOMPARE(item.textureUploadCount(), quint64{1});
    QCOMPARE(item.textureCount(), quint64{1});
    QCOMPARE(item.textureBytes(), quint64{8});

    TestImage ignoredReplacement;
    ignoredReplacement.rgba = QImage(QSize(2, 2), QImage::Format_RGBA8888);
    ignoredReplacement.rgba.fill(Qt::green);
    item.setImage(ignoredReplacement, 5);
    QVERIFY(!window.grabWindow().isNull());
    QCOMPARE(item.textureGeneration(), quint64{1});
    QCOMPARE(item.textureUploadCount(), quint64{1});
    QCOMPARE(item.textureCount(), quint64{1});
    QCOMPARE(item.textureBytes(), quint64{8});

    TestImage replacement;
    replacement.rgba = QImage(QSize(2, 2), QImage::Format_RGBA8888);
    replacement.rgba.fill(Qt::magenta);
    item.setImage(std::move(replacement), 6);
    QVERIFY(!window.grabWindow().isNull());
    QTRY_COMPARE_WITH_TIMEOUT(item.textureGeneration(), quint64{2}, 5'000);
    QCOMPARE(item.textureCount(), quint64{1});
    QCOMPARE(item.textureBytes(), quint64{16});
    item.synchronizeTelemetry();
    QVERIFY(!window.grabWindow().isNull());
    QTRY_COMPARE_WITH_TIMEOUT(item.textureUploadCount(), quint64{2}, 5'000);

    item.clearImage();
    QVERIFY(!window.grabWindow().isNull());
    QCOMPARE(item.textureUploadCount(), quint64{2});
    QCOMPARE(item.textureCount(), quint64{0});
    QCOMPARE(item.textureBytes(), quint64{0});
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariable("GHOSTTY_QT_BACKDROP_TEST_BACKEND")
        == QStringLiteral("vulkan")) {
        requestedGraphicsApi = QSGRendererInterface::Vulkan;
    }
    QQuickWindow::setGraphicsApi(requestedGraphicsApi);
    QGuiApplication application(argc, argv);
    TerminalBackdropRhiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_terminal_backdrop_rhi.moc"
