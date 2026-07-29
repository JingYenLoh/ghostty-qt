#include "terminal_kitty_graphics_qsg.h"

#include <QByteArray>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>

static void initializeTerminalKittyShaderResources()
{
    static const bool initialized = [] {
        Q_INIT_RESOURCE(ghostty_qt_terminal_kitty_shaders);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace {

constexpr qsizetype matrixOffset = 0;
constexpr qsizetype matrixSize = 64;
constexpr qsizetype inheritedOpacityOffset = 64;
constexpr qsizetype uniformBufferSize = 80;

void clearNodeChildren(QSGNode *node)
{
    if (node == nullptr) return;
    while (QSGNode *child = node->firstChild()) {
        node->removeChildNode(child);
        delete child;
    }
}

bool validImage(const TerminalKittyGraphicsImage &image)
{
    return image.generation != 0 && !image.straightRgbPlane.isNull()
        && image.straightRgbPlane.size() == image.alphaPlane.size()
        && image.straightRgbPlane.format() == QImage::Format_RGBX8888
        && image.alphaPlane.format() == QImage::Format_RGBX8888;
}

QImage premultipliedImage(const TerminalKittyGraphicsImage &asset)
{
    if (!validImage(asset)) return {};
    QImage result(asset.straightRgbPlane.size(),
                  QImage::Format_RGBA8888_Premultiplied);
    if (result.isNull()) return {};
    result.setDevicePixelRatio(1.0);
    for (int row = 0; row < result.height(); ++row) {
        const uchar *rgb = asset.straightRgbPlane.constScanLine(row);
        const uchar *alpha = asset.alphaPlane.constScanLine(row);
        uchar *destination = result.scanLine(row);
        for (int column = 0; column < result.width(); ++column) {
            const qsizetype offset = static_cast<qsizetype>(column) * 4;
            const quint32 a = alpha[offset];
            destination[offset] =
                static_cast<uchar>((rgb[offset] * a + 127U) / 255U);
            destination[offset + 1] =
                static_cast<uchar>((rgb[offset + 1] * a + 127U) / 255U);
            destination[offset + 2] =
                static_cast<uchar>((rgb[offset + 2] * a + 127U) / 255U);
            destination[offset + 3] = static_cast<uchar>(a);
        }
    }
    return result;
}

void configureTexture(QSGTexture &texture)
{
    texture.setFiltering(QSGTexture::Linear);
    texture.setMipmapFiltering(QSGTexture::None);
    texture.setHorizontalWrapMode(QSGTexture::ClampToEdge);
    texture.setVerticalWrapMode(QSGTexture::ClampToEdge);
}

class TerminalKittyQsgMaterial;

class TerminalKittyQsgShader final : public QSGMaterialShader {
public:
    TerminalKittyQsgShader()
    {
        initializeTerminalKittyShaderResources();
        setShaderFileName(
            VertexStage,
            QStringLiteral(":/ghostty-qt/shaders/terminal_kitty.vert.qsb"));
        setShaderFileName(
            FragmentStage,
            QStringLiteral(":/ghostty-qt/shaders/terminal_kitty.frag.qsb"));
    }

    bool updateUniformData(RenderState &state, QSGMaterial *,
                           QSGMaterial *oldMaterial) override
    {
        QByteArray &buffer = *state.uniformData();
        Q_ASSERT(buffer.size() >= uniformBufferSize);
        bool changed = false;
        if (oldMaterial == nullptr || state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            Q_ASSERT(matrixSize == static_cast<qsizetype>(sizeof(float) * 16));
            std::memcpy(buffer.data() + matrixOffset, matrix.constData(),
                        static_cast<std::size_t>(matrixSize));
            changed = true;
        }
        if (oldMaterial == nullptr || state.isOpacityDirty()) {
            const float opacity = state.opacity();
            std::memcpy(buffer.data() + inheritedOpacityOffset, &opacity,
                        sizeof(opacity));
            changed = true;
        }
        return changed;
    }

    void updateSampledImage(RenderState &state, int binding,
                            QSGTexture **texture, QSGMaterial *newMaterial,
                            QSGMaterial *) override;
};

class TerminalKittyQsgMaterial final : public QSGMaterial {
public:
    TerminalKittyQsgMaterial(QSGTexture *rgb, QSGTexture *alpha)
        : rgb_(rgb)
        , alpha_(alpha)
    {
        setFlag(QSGMaterial::Blending);
    }

    QSGMaterialType *type() const override
    {
        static QSGMaterialType materialType;
        return &materialType;
    }

    QSGMaterialShader *
    createShader(QSGRendererInterface::RenderMode) const override
    {
        return new TerminalKittyQsgShader;
    }

    int compare(const QSGMaterial *other) const override
    {
        const auto *right =
            static_cast<const TerminalKittyQsgMaterial *>(other);
        const std::less<QSGTexture *> less;
        if (rgb_ != right->rgb_) return less(rgb_, right->rgb_) ? -1 : 1;
        if (alpha_ != right->alpha_) {
            return less(alpha_, right->alpha_) ? -1 : 1;
        }
        return 0;
    }

    [[nodiscard]] QSGTexture *rgb() const noexcept { return rgb_; }
    [[nodiscard]] QSGTexture *alpha() const noexcept { return alpha_; }

private:
    QSGTexture *rgb_ = nullptr;
    QSGTexture *alpha_ = nullptr;
};

void TerminalKittyQsgShader::updateSampledImage(RenderState &state, int binding,
                                                QSGTexture **texture,
                                                QSGMaterial *newMaterial,
                                                QSGMaterial *)
{
    auto *material = static_cast<TerminalKittyQsgMaterial *>(newMaterial);
    QSGTexture *selected = binding == 1 ? material->rgb()
        : binding == 2                  ? material->alpha()
                                        : nullptr;
    Q_ASSERT(selected != nullptr);
    if (selected == nullptr) return;
    *texture = selected;
    selected->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
}

class TerminalKittyQsgNode final : public QSGGeometryNode {
public:
    TerminalKittyQsgNode(QSGTexture *rgb, QSGTexture *alpha,
                         const QRectF &destination, const QRectF &source)
    {
        auto *geometry = new QSGGeometry(
            QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
        geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        geometry->setVertexDataPattern(QSGGeometry::StaticPattern);
        const QRectF normalizedSource =
            rgb->convertToNormalizedSourceRect(source);
        QSGGeometry::updateTexturedRectGeometry(geometry, destination,
                                                normalizedSource);
        setGeometry(geometry);
        setFlag(QSGNode::OwnsGeometry);

        setMaterial(new TerminalKittyQsgMaterial(rgb, alpha));
        setFlag(QSGNode::OwnsMaterial);
    }
};

struct TextureSet {
    std::unique_ptr<QSGTexture> straightRgb;
    std::unique_ptr<QSGTexture> alpha;
    std::unique_ptr<QSGTexture> premultiplied;
};

QSGNode *parentForLayer(TerminalKittyGraphicsLayer layer,
                        QSGNode *belowBackground, QSGNode *belowText,
                        QSGNode *aboveText)
{
    switch (layer) {
    case TerminalKittyGraphicsLayer::BelowBackground: return belowBackground;
    case TerminalKittyGraphicsLayer::BelowText: return belowText;
    case TerminalKittyGraphicsLayer::AboveText: return aboveText;
    }
    return aboveText;
}

} // namespace

class TerminalKittyGraphicsScene::Impl final {
public:
    ~Impl() { clear(); }

    TextureSet *
    texture(QQuickWindow *quickWindow,
            const std::shared_ptr<const TerminalKittyGraphicsImage> &asset,
            bool useCustomMaterial)
    {
        if (asset == nullptr || !validImage(*asset)) return nullptr;
        if (auto found = textures.find(asset->generation);
            found != textures.end()) {
            return found->second.get();
        }

        auto result = std::make_unique<TextureSet>();
        if (useCustomMaterial) {
            result->straightRgb.reset(quickWindow->createTextureFromImage(
                asset->straightRgbPlane, QQuickWindow::TextureIsOpaque));
            result->alpha.reset(quickWindow->createTextureFromImage(
                asset->alphaPlane, QQuickWindow::TextureIsOpaque));
            if (!result->straightRgb || !result->alpha) return nullptr;
            configureTexture(*result->straightRgb);
            configureTexture(*result->alpha);
            textureUploadCount += 2;
        } else {
            const QImage image = premultipliedImage(*asset);
            if (image.isNull()) return nullptr;
            result->premultiplied.reset(
                quickWindow->createTextureFromImage(image));
            if (!result->premultiplied) return nullptr;
            configureTexture(*result->premultiplied);
            ++textureUploadCount;
        }

        TextureSet *pointer = result.get();
        textures.emplace(asset->generation, std::move(result));
        return pointer;
    }

    void clear()
    {
        clearNodeChildren(belowBackground);
        clearNodeChildren(belowText);
        clearNodeChildren(aboveText);
        textures.clear();
        rendered.clear();
        snapshot.reset();
        window = nullptr;
        belowBackground = nullptr;
        belowText = nullptr;
        aboveText = nullptr;
        cellSize = {};
        gridViewport = {};
    }

    QQuickWindow *window = nullptr;
    QSGNode *belowBackground = nullptr;
    QSGNode *belowText = nullptr;
    QSGNode *aboveText = nullptr;
    std::shared_ptr<const TerminalKittyGraphicsSnapshot> snapshot;
    QSizeF cellSize;
    QRectF gridViewport;
    bool custom = false;
    std::map<quint64, std::unique_ptr<TextureSet>> textures;
    QVector<TerminalKittyGraphicsRenderPlacement> rendered;
    quint64 textureUploadCount = 0;
};

TerminalKittyGraphicsScene::TerminalKittyGraphicsScene()
    : impl_(std::make_unique<Impl>())
{}

TerminalKittyGraphicsScene::~TerminalKittyGraphicsScene() = default;

void TerminalKittyGraphicsScene::update(
    QQuickWindow *window, QSGNode *belowBackground, QSGNode *belowText,
    QSGNode *aboveText,
    const std::shared_ptr<const TerminalKittyGraphicsSnapshot> &snapshot,
    const QSizeF &cellSize, const QRectF &gridViewport, bool useCustomMaterial)
{
    const bool parentsChanged = impl_->belowBackground != belowBackground
        || impl_->belowText != belowText || impl_->aboveText != aboveText;
    if (!parentsChanged && impl_->window == window
        && impl_->snapshot == snapshot && impl_->cellSize == cellSize
        && impl_->gridViewport == gridViewport
        && impl_->custom == useCustomMaterial) {
        return;
    }

    clearNodeChildren(impl_->belowBackground);
    clearNodeChildren(impl_->belowText);
    clearNodeChildren(impl_->aboveText);
    if (impl_->window != window || impl_->custom != useCustomMaterial) {
        impl_->textures.clear();
    }
    impl_->window = window;
    impl_->belowBackground = belowBackground;
    impl_->belowText = belowText;
    impl_->aboveText = aboveText;
    impl_->snapshot = snapshot;
    impl_->cellSize = cellSize;
    impl_->gridViewport = gridViewport;
    impl_->custom = useCustomMaterial;
    impl_->rendered.clear();

    std::set<quint64> activeTextures;
    bool materializedAllPlacements = true;
    if (window != nullptr && belowBackground != nullptr && belowText != nullptr
        && aboveText != nullptr && snapshot != nullptr
        && snapshot->cellWidthPixels != 0 && snapshot->cellHeightPixels != 0) {
        const QSizeF terminalCellPixelSize{
            static_cast<qreal>(snapshot->cellWidthPixels),
            static_cast<qreal>(snapshot->cellHeightPixels),
        };
        for (const TerminalKittyGraphicsPlacement &placement :
             snapshot->placements) {
            auto renderPlacement = terminalKittyGraphicsRenderPlacement(
                placement, cellSize, terminalCellPixelSize, gridViewport);
            if (!renderPlacement.has_value()
                || renderPlacement->image == nullptr) {
                continue;
            }
            TextureSet *textures = impl_->texture(
                window, renderPlacement->image, useCustomMaterial);
            if (textures == nullptr) {
                materializedAllPlacements = false;
                continue;
            }

            QSGNode *parent = parentForLayer(
                renderPlacement->layer, belowBackground, belowText, aboveText);
            if (useCustomMaterial) {
                parent->appendChildNode(new TerminalKittyQsgNode(
                    textures->straightRgb.get(), textures->alpha.get(),
                    renderPlacement->destination, renderPlacement->source));
            } else {
                auto *node = new QSGSimpleTextureNode;
                node->setOwnsTexture(false);
                node->setTexture(textures->premultiplied.get());
                node->setRect(renderPlacement->destination);
                node->setSourceRect(renderPlacement->source);
                parent->appendChildNode(node);
            }
            activeTextures.insert(renderPlacement->image->generation);
            impl_->rendered.append(std::move(*renderPlacement));
        }
    }

    for (auto iterator = impl_->textures.begin();
         iterator != impl_->textures.end();) {
        if (!activeTextures.contains(iterator->first)) {
            iterator = impl_->textures.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (!materializedAllPlacements) {
        // A transient graphics allocation failure must not permanently make
        // an unchanged terminal snapshot a no-op. Retry on the next paint
        // requested for any reason without scheduling an unbounded repaint.
        impl_->snapshot.reset();
    }
}

void TerminalKittyGraphicsScene::clear()
{
    impl_->clear();
}

const QVector<TerminalKittyGraphicsRenderPlacement> &
TerminalKittyGraphicsScene::renderedPlacements() const noexcept
{
    return impl_->rendered;
}

quint64 TerminalKittyGraphicsScene::textureUploadCount() const noexcept
{
    return impl_->textureUploadCount;
}

qsizetype TerminalKittyGraphicsScene::textureCount() const noexcept
{
    return static_cast<qsizetype>(impl_->textures.size());
}
