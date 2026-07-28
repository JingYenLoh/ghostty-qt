#include "terminal_backdrop_qsg.h"

#include <QFile>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {

struct TestPlanes {
    QImage rgb{QSize(2, 1), QImage::Format_RGBX8888};
    QImage alpha{QSize(2, 1), QImage::Format_RGBX8888};
};

void setPlanePixel(TestPlanes &planes, int x, int y, const QColor &rgb,
                   int alpha)
{
    planes.rgb.setPixelColor(x, y, rgb);
    planes.alpha.setPixelColor(x, y, QColor(alpha, alpha, alpha));
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

    void setPlanes(TestPlanes planes, quint64 serial)
    {
        planes_ = std::move(planes);
        serial_ = serial;
        update();
    }

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

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *) override
    {
        auto *node = oldNode != nullptr
            ? static_cast<TerminalBackdropQsgNode *>(oldNode)
            : new TerminalBackdropQsgNode;
        (void)node->update(
            window(), planes_.rgb, planes_.alpha, serial_, boundingRect(),
            background_, imageOpacity_, repeat_, destination_);
        textureGeneration_.store(node->textureGeneration(),
                                 std::memory_order_release);
        return node;
    }

private:
    TestPlanes planes_;
    quint64 serial_ = 1;
    QColor background_ = Qt::black;
    double imageOpacity_ = 1.0;
    bool repeat_ = false;
    QRectF destination_{0.0, 0.0, 2.0, 1.0};
    std::atomic<quint64> textureGeneration_{0};
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
    void repeatsWithAHardModuloSeam();
    void composesRelativeAndGlobalOpacity();
    void optionChangesDoNotUploadTexturesAgain();
};

void TerminalBackdropRhiTest::initTestCase()
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
}

void TerminalBackdropRhiTest::embedsShaderResources()
{
    QVERIFY(QFile::exists(QStringLiteral(
        ":/ghostty-qt/shaders/terminal_backdrop.vert.qsb")));
    QVERIFY(QFile::exists(QStringLiteral(
        ":/ghostty-qt/shaders/terminal_backdrop.frag.qsb")));
}

void TerminalBackdropRhiTest::samplesStraightAlphaBeforePremultiplication()
{
    TestPlanes planes;
    setPlanePixel(planes, 0, 0, Qt::red, 255);
    setPlanePixel(planes, 1, 0, Qt::green, 0);

    QQuickWindow window;
    BackdropItem item;
    item.setPlanes(std::move(planes), 1);
    item.setComposition(Qt::black, 1.0, false,
                        QRectF(0.0, 0.0, 4.0, 1.0));
    const QImage frame = render(item, window, QSize(4, 1));
    if (frame.isNull()) {
        QSKIP("The managed environment has no usable Qt RHI backend.");
    }
    if (!QSGRendererInterface::isApiRhiBased(
            window.rendererInterface()->graphicsApi())) {
        QSKIP("The offscreen Qt platform selected the software adaptation.");
    }

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

void TerminalBackdropRhiTest::repeatsWithAHardModuloSeam()
{
    TestPlanes planes;
    setPlanePixel(planes, 0, 0, Qt::red, 255);
    setPlanePixel(planes, 1, 0, Qt::blue, 255);

    QQuickWindow window;
    BackdropItem item;
    item.setPlanes(std::move(planes), 2);
    item.setComposition(Qt::black, 1.0, true,
                        QRectF(0.0, 0.0, 8.0, 1.0));
    const QImage frame = render(item, window, QSize(16, 1));
    if (frame.isNull()) {
        QSKIP("The managed environment has no usable Qt RHI backend.");
    }
    if (!QSGRendererInterface::isApiRhiBased(
            window.rendererInterface()->graphicsApi())) {
        QSKIP("The offscreen Qt platform selected the software adaptation.");
    }

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
    TestPlanes planes;
    setPlanePixel(planes, 0, 0, Qt::red, 255);
    setPlanePixel(planes, 1, 0, Qt::red, 255);

    QQuickWindow window;
    BackdropItem item;
    item.setPlanes(std::move(planes), 3);
    item.setComposition(QColor(0, 0, 255, 128), 1.5, false,
                        QRectF(0.0, 0.0, 4.0, 2.0));
    QImage frame = render(item, window, QSize(4, 2));
    if (frame.isNull()) {
        QSKIP("The managed environment has no usable Qt RHI backend.");
    }
    if (!QSGRendererInterface::isApiRhiBased(
            window.rendererInterface()->graphicsApi())) {
        QSKIP("The offscreen Qt platform selected the software adaptation.");
    }

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
    TestPlanes planes;
    setPlanePixel(planes, 0, 0, Qt::red, 255);
    setPlanePixel(planes, 1, 0, Qt::blue, 255);

    QQuickWindow window;
    BackdropItem item;
    item.setPlanes(planes, 4);
    item.setComposition(QColor(0, 0, 0, 128), 1.0, false,
                        QRectF(0.0, 0.0, 2.0, 1.0));
    if (render(item, window, QSize(6, 1)).isNull()) {
        QSKIP("The managed environment has no usable Qt RHI backend.");
    }
    if (!QSGRendererInterface::isApiRhiBased(
            window.rendererInterface()->graphicsApi())) {
        QSKIP("The offscreen Qt platform selected the software adaptation.");
    }
    QTRY_COMPARE_WITH_TIMEOUT(item.textureGeneration(), quint64{1}, 5'000);

    item.setComposition(QColor(10, 20, 30, 192), 1.5, true,
                        QRectF(2.0, 0.0, 2.0, 1.0));
    QVERIFY(!window.grabWindow().isNull());
    QCOMPARE(item.textureGeneration(), quint64{1});

    item.setPlanes(std::move(planes), 5);
    QVERIFY(!window.grabWindow().isNull());
    QTRY_COMPARE_WITH_TIMEOUT(item.textureGeneration(), quint64{2}, 5'000);
}

QTEST_MAIN(TerminalBackdropRhiTest)

#include "test_terminal_backdrop_rhi.moc"
