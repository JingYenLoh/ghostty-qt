#include "terminal_glyph_batch.h"

#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QTest>
#include <QtMath>

#include <atomic>
#include <cmath>
#include <memory>

namespace {

QSGRendererInterface::GraphicsApi requestedGraphicsApi =
    QSGRendererInterface::OpenGL;

class GlyphBatchNode final : public QSGNode {
public:
    GlyphBatchNode(QQuickWindow *window, const QImage &coverage,
                   const QRectF &destination, const QColor &color)
        : batch_(new TerminalGlyphBatch)
    {
        appendChildNode(batch_);
        texture_.reset(window != nullptr
                           ? window->createTextureFromImage(
                                 coverage, QQuickWindow::TextureHasAlphaChannel)
                           : nullptr);
        if (texture_ == nullptr) return;

        texture_->setFiltering(QSGTexture::Nearest);
        texture_->setMipmapFiltering(QSGTexture::None);
        texture_->setHorizontalWrapMode(QSGTexture::ClampToEdge);
        texture_->setVerticalWrapMode(QSGTexture::ClampToEdge);

        QVector<TerminalGlyphQuad> &glyphs = batch_->beginUpdate();
        glyphs.append({
            .destination = destination,
            .normalizedSource = texture_->convertToNormalizedSourceRect(
                QRectF(QPointF{}, QSizeF(coverage.size()))),
            .color = color,
        });
        result_ = batch_->commit(texture_.get());
    }

    ~GlyphBatchNode() override
    {
        removeChildNode(batch_);
        delete batch_;
    }

    [[nodiscard]] TerminalGlyphBatchCommitResult result() const noexcept
    {
        return result_;
    }

private:
    TerminalGlyphBatch *batch_ = nullptr;
    std::unique_ptr<QSGTexture> texture_;
    TerminalGlyphBatchCommitResult result_ =
        TerminalGlyphBatchCommitResult::Fallback;
};

class GlyphBatchItem final : public QQuickItem {
public:
    GlyphBatchItem()
    {
        setFlag(QQuickItem::ItemHasContents);

        coverage_ = QImage(QSize(2, 1), QImage::Format_RGBA8888_Premultiplied);
        coverage_.setPixelColor(0, 0, QColor(0, 0, 0, 0));
        coverage_.setPixelColor(1, 0, QColor(255, 255, 255, 128));
    }

    [[nodiscard]] TerminalGlyphBatchCommitResult result() const noexcept
    {
        return static_cast<TerminalGlyphBatchCommitResult>(
            result_.load(std::memory_order_acquire));
    }

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override
    {
        if (oldNode != nullptr) return oldNode;

        auto *const node = new GlyphBatchNode(
            window(), coverage_, QRectF(2.0, 2.0, 20.0, 8.0), glyphColor_);
        result_.store(static_cast<int>(node->result()),
                      std::memory_order_release);
        return node;
    }

private:
    QImage coverage_;
    QColor glyphColor_{180, 100, 40};
    std::atomic<int> result_{
        static_cast<int>(TerminalGlyphBatchCommitResult::Fallback)};
};

[[nodiscard]] bool near(int actual, int expected, int tolerance = 12)
{
    return std::abs(actual - expected) <= tolerance;
}

[[nodiscard]] QString colorDescription(const QColor &color)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

[[nodiscard]] QImage render(GlyphBatchItem &item, QQuickWindow &window)
{
    constexpr QSize logicalSize{24, 12};
    window.setColor(Qt::black);
    window.resize(logicalSize);
    item.setParentItem(window.contentItem());
    item.setSize(logicalSize);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window, 5'000)) return {};

    QImage frame;
    for (int attempt = 0; attempt < 50 && frame.isNull(); ++attempt) {
        frame = window.grabWindow();
        if (frame.isNull()) QTest::qWait(20);
    }
    return frame;
}

[[nodiscard]] QPoint physicalSample(const QQuickWindow &window,
                                    const QImage &frame, QPointF logical)
{
    const qreal dpr = window.devicePixelRatio();
    return {
        qBound(0, qFloor(logical.x() * dpr), frame.width() - 1),
        qBound(0, qFloor(logical.y() * dpr), frame.height() - 1),
    };
}

} // namespace

class TerminalGlyphBatchRhiTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void rendersCoverageTexture();
};

void TerminalGlyphBatchRhiTest::rendersCoverageTexture()
{
    QQuickWindow window;
    GlyphBatchItem item;
    const QImage frame = render(item, window);
    if (frame.isNull()) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    const QSGRendererInterface::GraphicsApi actualGraphicsApi =
        window.rendererInterface()->graphicsApi();
    if (!QSGRendererInterface::isApiRhiBased(actualGraphicsApi)
        || actualGraphicsApi != requestedGraphicsApi) {
        QSKIP("The platform cannot initialize the requested RHI context.");
    }
    QCOMPARE(item.result(), TerminalGlyphBatchCommitResult::Batched);

    const QColor transparent =
        frame.pixelColor(physicalSample(window, frame, QPointF(6.0, 6.0)));
    QVERIFY2(transparent.red() < 8 && transparent.green() < 8
                 && transparent.blue() < 8,
             qPrintable(QStringLiteral("transparent atlas texel drew %1")
                            .arg(colorDescription(transparent))));

    const QColor covered =
        frame.pixelColor(physicalSample(window, frame, QPointF(18.0, 6.0)));
    constexpr int coverage = 128;
    const QColor glyphColor(180, 100, 40);
    const auto coveredComponent = [](int component) {
        return (component * coverage + 127) / 255;
    };
    const QColor expected(coveredComponent(glyphColor.red()),
                          coveredComponent(glyphColor.green()),
                          coveredComponent(glyphColor.blue()));
    QVERIFY2(near(covered.red(), expected.red())
                 && near(covered.green(), expected.green())
                 && near(covered.blue(), expected.blue()),
             qPrintable(QStringLiteral("coverage sample %1, expected %2")
                            .arg(colorDescription(covered))
                            .arg(colorDescription(expected))));
    QVERIFY2(covered.red() < glyphColor.red() - 40,
             "Coverage sampling unexpectedly produced a solid glyph quad.");
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariable("GHOSTTY_QT_GLYPH_BATCH_TEST_BACKEND")
        == QStringLiteral("vulkan")) {
        requestedGraphicsApi = QSGRendererInterface::Vulkan;
    }
    QQuickWindow::setGraphicsApi(requestedGraphicsApi);
    QGuiApplication application(argc, argv);
    TerminalGlyphBatchRhiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_terminal_glyph_batch_rhi.moc"
