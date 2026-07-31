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
#include <tuple>
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

using namespace TerminalKittyGraphicsShaderLayout;

static_assert(matrixSize == 64);
static_assert(uniformBufferSize == 68);

bool validImage(const TerminalKittyGraphicsImage &image)
{
    if (image.generation == 0 || image.straightRgbPlane.isNull()
        || image.straightRgbPlane.format() != QImage::Format_RGBX8888) {
        return false;
    }
    return image.fullyOpaque ? image.alphaPlane.isNull()
                             : !image.alphaPlane.isNull()
            && image.straightRgbPlane.size() == image.alphaPlane.size()
            && image.alphaPlane.format() == QImage::Format_RGBX8888;
}

QImage premultipliedImage(const TerminalKittyGraphicsImage &asset)
{
    if (!validImage(asset)) return {};
    if (asset.fullyOpaque) return asset.straightRgbPlane;
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

    bool setTextures(QSGTexture *rgb, QSGTexture *alpha) noexcept
    {
        if (rgb_ == rgb && alpha_ == alpha) return false;
        rgb_ = rgb;
        alpha_ = alpha;
        return true;
    }

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

struct PlacementNodeUpdate {
    bool geometryChanged = false;
    bool materialChanged = false;
};

class TerminalKittyQsgNode final : public QSGGeometryNode {
public:
    TerminalKittyQsgNode(QSGTexture *rgb, QSGTexture *alpha,
                         const QRectF &destination, const QRectF &source)
        : geometry_(new QSGGeometry(
              QSGGeometry::defaultAttributes_TexturedPoint2D(), 4))
        , material_(new TerminalKittyQsgMaterial(rgb, alpha))
    {
        geometry_->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        geometry_->setVertexDataPattern(QSGGeometry::DynamicPattern);
        destination_ = destination;
        normalizedSource_ = rgb->convertToNormalizedSourceRect(source);
        QSGGeometry::updateTexturedRectGeometry(geometry_, destination_,
                                                normalizedSource_);
        setGeometry(geometry_);
        setFlag(QSGNode::OwnsGeometry);

        setMaterial(material_);
        setFlag(QSGNode::OwnsMaterial);
    }

    PlacementNodeUpdate update(QSGTexture *rgb, QSGTexture *alpha,
                               const QRectF &destination, const QRectF &source)
    {
        PlacementNodeUpdate result;
        const QRectF normalizedSource =
            rgb->convertToNormalizedSourceRect(source);
        if (destination_ != destination
            || normalizedSource_ != normalizedSource) {
            destination_ = destination;
            normalizedSource_ = normalizedSource;
            QSGGeometry::updateTexturedRectGeometry(geometry_, destination_,
                                                    normalizedSource_);
            markDirty(QSGNode::DirtyGeometry);
            result.geometryChanged = true;
        }
        if (material_->setTextures(rgb, alpha)) {
            markDirty(QSGNode::DirtyMaterial);
            result.materialChanged = true;
        }
        return result;
    }

private:
    QSGGeometry *geometry_ = nullptr;
    TerminalKittyQsgMaterial *material_ = nullptr;
    QRectF destination_;
    QRectF normalizedSource_;
};

struct TextureSet {
    std::unique_ptr<QSGTexture> straightRgb;
    std::unique_ptr<QSGTexture> alpha;
    QSGTexture *sampledAlpha = nullptr;
    std::unique_ptr<QSGTexture> premultiplied;
};

struct PlacementKey {
    quint32 imageId = 0;
    quint32 placementId = 0;
    TerminalKittyGraphicsLayer layer = TerminalKittyGraphicsLayer::AboveText;

    bool operator==(const PlacementKey &) const noexcept = default;

    bool operator<(const PlacementKey &other) const noexcept
    {
        return std::tie(imageId, placementId, layer)
            < std::tie(other.imageId, other.placementId, other.layer);
    }
};

struct PlacementNode {
    PlacementKey key;
    quint64 generation = 0;
    qint32 z = 0;
    QRectF destination;
    QRectF source;
    QSGNode *node = nullptr;
};

struct MaterializedPlacement {
    TerminalKittyGraphicsRenderPlacement placement;
    TextureSet *textures = nullptr;
};

using RectSignature = std::array<qreal, 4>;
using ExactCandidateSignature =
    std::tuple<quint64, qint32, RectSignature, RectSignature>;
using GenerationZSourceSignature = std::tuple<quint64, qint32, RectSignature>;
using ZGeometrySignature = std::tuple<qint32, RectSignature, RectSignature>;
using ZSourceSignature = std::tuple<qint32, RectSignature>;

RectSignature rectSignature(const QRectF &rect) noexcept
{
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}

struct CandidateQueue {
    QVector<qsizetype> indices;
    qsizetype cursor = 0;

    qsizetype take(const QVector<char> &consumed)
    {
        while (cursor < indices.size()
               && consumed.at(indices.at(cursor)) != 0) {
            ++cursor;
        }
        if (cursor >= indices.size()) return -1;
        return indices.at(cursor++);
    }
};

struct PlacementCandidateBucket {
    CandidateQueue any;
    std::map<ExactCandidateSignature, CandidateQueue> exact;
    std::map<GenerationZSourceSignature, CandidateQueue> generationZSource;
    std::map<quint64, CandidateQueue> generation;
    std::map<ZGeometrySignature, CandidateQueue> zGeometry;
    std::map<ZSourceSignature, CandidateQueue> zSource;

    void append(qsizetype index, const PlacementNode &candidate)
    {
        const RectSignature destination = rectSignature(candidate.destination);
        const RectSignature source = rectSignature(candidate.source);
        any.indices.append(index);
        exact[{candidate.generation, candidate.z, destination, source}]
            .indices.append(index);
        generationZSource[{candidate.generation, candidate.z, source}]
            .indices.append(index);
        generation[candidate.generation].indices.append(index);
        zGeometry[{candidate.z, destination, source}].indices.append(index);
        zSource[{candidate.z, source}].indices.append(index);
    }

    qsizetype take(const TerminalKittyGraphicsRenderPlacement &desired,
                   const QVector<char> &consumed)
    {
        const RectSignature destination = rectSignature(desired.destination);
        const RectSignature source = rectSignature(desired.source);
        const auto takeFrom = [&consumed](auto &index, const auto &signature) {
            const auto found = index.find(signature);
            return found != index.end() ? found->second.take(consumed) : -1;
        };

        qsizetype selected =
            takeFrom(exact,
                     ExactCandidateSignature{desired.image->generation,
                                             desired.z, destination, source});
        if (selected >= 0) return selected;
        selected = takeFrom(generationZSource,
                            GenerationZSourceSignature{
                                desired.image->generation, desired.z, source});
        if (selected >= 0) return selected;
        selected = takeFrom(generation, desired.image->generation);
        if (selected >= 0) return selected;
        selected = takeFrom(zGeometry,
                            ZGeometrySignature{desired.z, destination, source});
        if (selected >= 0) return selected;
        selected = takeFrom(zSource, ZSourceSignature{desired.z, source});
        return selected >= 0 ? selected : any.take(consumed);
    }
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

    void clearPlacementNodes()
    {
        quint64 removed = 0;
        for (QSGNode *parent :
             std::array{belowBackground, belowText, aboveText}) {
            if (parent == nullptr) continue;
            while (QSGNode *node = parent->firstChild()) {
                parent->removeChildNode(node);
                delete node;
                ++removed;
            }
        }
        Q_ASSERT(removed == static_cast<quint64>(placementNodes.size()));
        nodeDeletionCount += removed;
        placementNodes.clear();
    }

    void clearTextures()
    {
        textureSetEvictionCount += static_cast<quint64>(textures.size());
        textures.clear();
    }

    void evictInactiveTextures(const std::set<quint64> &activeTextures)
    {
        for (auto iterator = textures.begin(); iterator != textures.end();) {
            if (!activeTextures.contains(iterator->first)) {
                iterator = textures.erase(iterator);
                ++textureSetEvictionCount;
            } else {
                ++iterator;
            }
        }
    }

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
            if (asset->fullyOpaque) {
                if (opaqueAlpha == nullptr) {
                    QImage white(1, 1, QImage::Format_RGBX8888);
                    white.fill(Qt::white);
                    opaqueAlpha.reset(quickWindow->createTextureFromImage(
                        white, QQuickWindow::TextureIsOpaque));
                    if (opaqueAlpha == nullptr) return nullptr;
                    configureTexture(*opaqueAlpha);
                    ++textureUploadCount;
                }
                result->sampledAlpha = opaqueAlpha.get();
            } else {
                result->alpha.reset(quickWindow->createTextureFromImage(
                    asset->alphaPlane, QQuickWindow::TextureIsOpaque));
                if (result->alpha == nullptr) return nullptr;
                configureTexture(*result->alpha);
                result->sampledAlpha = result->alpha.get();
                ++textureUploadCount;
            }
            if (!result->straightRgb) return nullptr;
            configureTexture(*result->straightRgb);
            ++textureUploadCount;
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

    PlacementNode createPlacementNode(const MaterializedPlacement &materialized,
                                      bool useCustomMaterial)
    {
        const TerminalKittyGraphicsRenderPlacement &placement =
            materialized.placement;
        QSGNode *node = nullptr;
        if (useCustomMaterial) {
            node = new TerminalKittyQsgNode(
                materialized.textures->straightRgb.get(),
                materialized.textures->sampledAlpha, placement.destination,
                placement.source);
        } else {
            auto *textureNode = new QSGSimpleTextureNode;
            textureNode->setOwnsTexture(false);
            textureNode->setTexture(materialized.textures->premultiplied.get());
            textureNode->setRect(placement.destination);
            textureNode->setSourceRect(placement.source);
            node = textureNode;
        }
        ++nodeCreationCount;
        ++geometryWriteCount;
        ++materialAssignmentCount;
        return {
            .key =
                {
                    .imageId = placement.image->imageId,
                    .placementId = placement.placementId,
                    .layer = placement.layer,
                },
            .generation = placement.image->generation,
            .z = placement.z,
            .destination = placement.destination,
            .source = placement.source,
            .node = node,
        };
    }

    PlacementNodeUpdate
    updatePlacementNode(PlacementNode &node,
                        const MaterializedPlacement &materialized,
                        bool useCustomMaterial)
    {
        const TerminalKittyGraphicsRenderPlacement &placement =
            materialized.placement;
        PlacementNodeUpdate result;
        if (useCustomMaterial) {
            result = static_cast<TerminalKittyQsgNode *>(node.node)->update(
                materialized.textures->straightRgb.get(),
                materialized.textures->sampledAlpha, placement.destination,
                placement.source);
        } else {
            auto *textureNode = static_cast<QSGSimpleTextureNode *>(node.node);
            QSGTexture *const oldTexture = textureNode->texture();
            QSGTexture *const newTexture =
                materialized.textures->premultiplied.get();
            const bool textureSizeChanged = oldTexture != nullptr
                && oldTexture->textureSize() != newTexture->textureSize();
            result.materialChanged = oldTexture != newTexture;
            result.geometryChanged =
                textureNode->rect() != placement.destination
                || textureNode->sourceRect() != placement.source
                || (result.materialChanged && textureSizeChanged);
            if (result.materialChanged) {
                textureNode->setTexture(newTexture);
            }
            if (textureNode->rect() != placement.destination) {
                textureNode->setRect(placement.destination);
            }
            if (textureNode->sourceRect() != placement.source
                || (result.materialChanged && textureSizeChanged)) {
                textureNode->setSourceRect(placement.source);
            }
        }
        geometryWriteCount += static_cast<quint64>(result.geometryChanged);
        materialAssignmentCount += static_cast<quint64>(result.materialChanged);
        node.key = {
            .imageId = placement.image->imageId,
            .placementId = placement.placementId,
            .layer = placement.layer,
        };
        node.generation = placement.image->generation;
        node.z = placement.z;
        node.destination = placement.destination;
        node.source = placement.source;
        return result;
    }

    void orderPlacementNodes()
    {
        constexpr std::array layers{
            TerminalKittyGraphicsLayer::BelowBackground,
            TerminalKittyGraphicsLayer::BelowText,
            TerminalKittyGraphicsLayer::AboveText,
        };
        for (const TerminalKittyGraphicsLayer layer : layers) {
            QSGNode *const parent =
                parentForLayer(layer, belowBackground, belowText, aboveText);
            if (parent == nullptr) continue;
            QSGNode *previous = nullptr;
            for (const PlacementNode &placement : placementNodes) {
                if (placement.key.layer != layer) continue;
                QSGNode *const expected = previous != nullptr
                    ? previous->nextSibling()
                    : parent->firstChild();
                if (expected != placement.node) {
                    if (QSGNode *currentParent = placement.node->parent()) {
                        currentParent->removeChildNode(placement.node);
                    }
                    if (expected != nullptr) {
                        parent->insertChildNodeBefore(placement.node, expected);
                    } else {
                        parent->appendChildNode(placement.node);
                    }
                }
                previous = placement.node;
            }
        }
    }

    void
    reconcilePlacementNodes(const QVector<MaterializedPlacement> &materialized,
                            bool useCustomMaterial)
    {
        if (materialized.isEmpty()) {
            clearPlacementNodes();
            return;
        }
        if (placementNodes.isEmpty()) {
            placementNodes.reserve(materialized.size());
            for (const MaterializedPlacement &desired : materialized) {
                PlacementNode node =
                    createPlacementNode(desired, useCustomMaterial);
                parentForLayer(node.key.layer, belowBackground, belowText,
                               aboveText)
                    ->appendChildNode(node.node);
                placementNodes.append(std::move(node));
            }
            return;
        }

        // Explicit placement IDs normally keep a stable one-to-one order.
        // Avoid allocating lookup buckets on that hot path. Implicit p=0
        // placements can collide after crossing libghostty's public API, so
        // they always use the duplicate-aware matcher below.
        bool canUpdateInOrder = placementNodes.size() == materialized.size();
        if (canUpdateInOrder) {
            for (qsizetype index = 0; index < materialized.size(); ++index) {
                const TerminalKittyGraphicsRenderPlacement &placement =
                    materialized.at(index).placement;
                const PlacementKey key{
                    .imageId = placement.image->imageId,
                    .placementId = placement.placementId,
                    .layer = placement.layer,
                };
                if (key.placementId == 0
                    || placementNodes.at(index).key != key) {
                    canUpdateInOrder = false;
                    break;
                }
            }
        }
        if (canUpdateInOrder) {
            for (qsizetype index = 0; index < materialized.size(); ++index) {
                updatePlacementNode(placementNodes[index],
                                    materialized.at(index), useCustomMaterial);
            }
            return;
        }

        std::map<PlacementKey, PlacementCandidateBucket> candidates;
        for (qsizetype index = 0; index < placementNodes.size(); ++index) {
            candidates[placementNodes.at(index).key].append(
                index, placementNodes.at(index));
        }
        QVector<char> consumed(placementNodes.size(), 0);
        QVector<PlacementNode> next;
        next.reserve(materialized.size());
        for (const MaterializedPlacement &desired : materialized) {
            const PlacementKey key{
                .imageId = desired.placement.image->imageId,
                .placementId = desired.placement.placementId,
                .layer = desired.placement.layer,
            };
            qsizetype selected = -1;
            if (const auto found = candidates.find(key);
                found != candidates.end()) {
                selected = found->second.take(desired.placement, consumed);
            }
            if (selected < 0) {
                next.append(createPlacementNode(desired, useCustomMaterial));
                continue;
            }
            consumed[selected] = 1;
            PlacementNode retained = placementNodes.at(selected);
            updatePlacementNode(retained, desired, useCustomMaterial);
            next.append(std::move(retained));
        }

        for (qsizetype index = 0; index < placementNodes.size(); ++index) {
            if (consumed.at(index) != 0) continue;
            QSGNode *const node = placementNodes.at(index).node;
            if (QSGNode *parent = node->parent()) {
                parent->removeChildNode(node);
            }
            delete node;
            ++nodeDeletionCount;
        }
        placementNodes = std::move(next);
        orderPlacementNodes();
    }

    void clear()
    {
        clearPlacementNodes();
        clearTextures();
        opaqueAlpha.reset();
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
    std::unique_ptr<QSGTexture> opaqueAlpha;
    QVector<PlacementNode> placementNodes;
    QVector<TerminalKittyGraphicsRenderPlacement> rendered;
    quint64 textureUploadCount = 0;
    quint64 nodeCreationCount = 0;
    quint64 nodeDeletionCount = 0;
    quint64 geometryWriteCount = 0;
    quint64 materialAssignmentCount = 0;
    quint64 textureSetEvictionCount = 0;
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

    const bool windowChanged = impl_->window != window;
    const bool materialPathChanged = impl_->custom != useCustomMaterial;
    if (parentsChanged || windowChanged || materialPathChanged) {
        impl_->clearPlacementNodes();
        impl_->rendered.clear();
        impl_->snapshot.reset();
        if (windowChanged || materialPathChanged) {
            impl_->clearTextures();
            impl_->opaqueAlpha.reset();
        }
    }
    impl_->window = window;
    impl_->belowBackground = belowBackground;
    impl_->belowText = belowText;
    impl_->aboveText = aboveText;
    impl_->cellSize = cellSize;
    impl_->gridViewport = gridViewport;
    impl_->custom = useCustomMaterial;

    std::set<quint64> activeTextures;
    QVector<MaterializedPlacement> materialized;
    QVector<TerminalKittyGraphicsRenderPlacement> rendered;
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

            activeTextures.insert(renderPlacement->image->generation);
            rendered.append(*renderPlacement);
            materialized.append({
                .placement = std::move(*renderPlacement),
                .textures = textures,
            });
        }
    }

    impl_->reconcilePlacementNodes(materialized, useCustomMaterial);
    impl_->evictInactiveTextures(activeTextures);
    impl_->rendered = std::move(rendered);
    impl_->snapshot = snapshot;
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

quint64 TerminalKittyGraphicsScene::nodeCreationCount() const noexcept
{
    return impl_->nodeCreationCount;
}

quint64 TerminalKittyGraphicsScene::nodeDeletionCount() const noexcept
{
    return impl_->nodeDeletionCount;
}

quint64 TerminalKittyGraphicsScene::geometryWriteCount() const noexcept
{
    return impl_->geometryWriteCount;
}

quint64 TerminalKittyGraphicsScene::materialAssignmentCount() const noexcept
{
    return impl_->materialAssignmentCount;
}

quint64 TerminalKittyGraphicsScene::textureSetEvictionCount() const noexcept
{
    return impl_->textureSetEvictionCount;
}

qsizetype TerminalKittyGraphicsScene::textureCount() const noexcept
{
    return static_cast<qsizetype>(impl_->textures.size());
}
