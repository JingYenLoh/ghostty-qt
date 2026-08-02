#include "terminal_backdrop_qsg.h"
#include "terminal_straight_rgba_texture_p.h"

#include <QByteArray>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QResource>
#include <QSGGeometry>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <ranges>
#include <type_traits>
#include <utility>

static void initializeTerminalBackdropShaderResources()
{
    static const bool initialized = [] {
        Q_INIT_RESOURCE(ghostty_qt_terminal_backdrop_shaders);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace {

constexpr qsizetype matrixOffset = 0;
constexpr qsizetype matrixSize = 64;
constexpr qsizetype inheritedOpacityOffset = 64;
constexpr qsizetype imageOpacityOffset = 68;
constexpr qsizetype repeatOffset = 72;
constexpr qsizetype linearBlendingOffset = 76;
constexpr qsizetype backgroundOffset = 80;
constexpr qsizetype destinationOffset = 96;
constexpr qsizetype uniformBufferSize = 112;

[[nodiscard]] bool finiteRect(const QRectF &rect) noexcept
{
    return std::isfinite(rect.x()) && std::isfinite(rect.y())
        && std::isfinite(rect.width()) && std::isfinite(rect.height())
        && rect.width() > 0.0 && rect.height() > 0.0;
}

[[nodiscard]] QRectF mappedSourceRect(const QRectF &visible,
                                      const QRectF &destination,
                                      const QSize &sourcePixels) noexcept
{
    return {
        (visible.left() - destination.left()) / destination.width()
            * sourcePixels.width(),
        (visible.top() - destination.top()) / destination.height()
            * sourcePixels.height(),
        visible.width() / destination.width() * sourcePixels.width(),
        visible.height() / destination.height() * sourcePixels.height(),
    };
}

[[nodiscard]] bool validStraightRgba(const QImage &straightRgba) noexcept
{
    return !straightRgba.isNull()
        && straightRgba.format() == QImage::Format_RGBA8888;
}

QImage linearizedPremultipliedImage(QImage image,
                                    TerminalAlphaBlending alphaBlending)
{
    if (!terminalUsesLinearBlending(alphaBlending) || image.isNull()) {
        return image;
    }
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const auto byte = [](float component) {
        return std::clamp(static_cast<int>(std::lround(component * 255.0F)), 0,
                          255);
    };
    for (int y = 0; y < image.height(); ++y) {
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int alphaByte = qAlpha(pixels[x]);
            if (alphaByte == 0) {
                pixels[x] = 0;
                continue;
            }
            const float alpha = static_cast<float>(alphaByte) / 255.0F;
            const auto channel = [alpha, alphaByte, &byte](int premultiplied) {
                const float straight =
                    std::clamp(static_cast<float>(premultiplied)
                                   / static_cast<float>(alphaByte),
                               0.0F, 1.0F);
                return byte(terminalSrgbToLinear(straight) * alpha);
            };
            pixels[x] =
                qRgba(channel(qRed(pixels[x])), channel(qGreen(pixels[x])),
                      channel(qBlue(pixels[x])), alphaByte);
        }
    }
    return image;
}

template <typename Value>
void writeUniform(QByteArray &buffer, qsizetype offset, const Value &value)
{
    static_assert(std::is_trivially_copyable_v<Value>);
    Q_ASSERT(offset >= 0);
    Q_ASSERT(offset + static_cast<qsizetype>(sizeof(Value)) <= buffer.size());
    std::memcpy(buffer.data() + offset, &value, sizeof(Value));
}

template <std::size_t Size>
void writeUniform(QByteArray &buffer, qsizetype offset,
                  const std::array<float, Size> &value)
{
    Q_ASSERT(offset >= 0);
    Q_ASSERT(offset + static_cast<qsizetype>(sizeof(float) * value.size())
             <= buffer.size());
    std::memcpy(buffer.data() + offset, value.data(),
                sizeof(float) * value.size());
}

class TerminalBackdropQsgShader final : public QSGMaterialShader {
public:
    TerminalBackdropQsgShader();

    bool updateUniformData(RenderState &state, QSGMaterial *newMaterial,
                           QSGMaterial *oldMaterial) override;
    void updateSampledImage(RenderState &state, int binding,
                            QSGTexture **texture, QSGMaterial *newMaterial,
                            QSGMaterial *oldMaterial) override;
};

} // namespace

class TerminalBackdropQsgMaterial final : public QSGMaterial {
public:
    TerminalBackdropQsgMaterial() { setFlag(QSGMaterial::Blending); }

    QSGMaterialType *type() const override
    {
        static QSGMaterialType materialType;
        return &materialType;
    }

    QSGMaterialShader *
    createShader(QSGRendererInterface::RenderMode) const override
    {
        return new TerminalBackdropQsgShader;
    }

    [[nodiscard]] bool
    setTexture(TerminalStraightRgbaTexture *straightRgba) noexcept
    {
        if (straightRgba_ == straightRgba) return false;
        straightRgba_ = straightRgba;
        return true;
    }

    [[nodiscard]] bool setComposition(const QColor &background,
                                      float imageOpacity, bool repeat,
                                      const QRectF &destination,
                                      TerminalAlphaBlending alphaBlending)
    {
        const QColor rgb = background.toRgb();
        const std::array<float, 4> nextBackground{
            static_cast<float>(rgb.redF()),
            static_cast<float>(rgb.greenF()),
            static_cast<float>(rgb.blueF()),
            static_cast<float>(rgb.alphaF()),
        };
        const std::array<float, 4> nextDestination{
            static_cast<float>(destination.x()),
            static_cast<float>(destination.y()),
            static_cast<float>(destination.width()),
            static_cast<float>(destination.height()),
        };
        const float nextRepeat = repeat ? 1.0F : 0.0F;
        const float nextLinearBlending =
            terminalUsesLinearBlending(alphaBlending) ? 1.0F : 0.0F;
        if (background_ == nextBackground && destination_ == nextDestination
            && imageOpacity_ == imageOpacity && repeat_ == nextRepeat
            && linearBlending_ == nextLinearBlending) {
            return false;
        }
        background_ = nextBackground;
        destination_ = nextDestination;
        imageOpacity_ = imageOpacity;
        repeat_ = nextRepeat;
        linearBlending_ = nextLinearBlending;
        uniformsDirty_ = true;
        return true;
    }

    [[nodiscard]] bool takeUniformsDirty() const noexcept
    {
        return std::exchange(uniformsDirty_, false);
    }

    [[nodiscard]] TerminalStraightRgbaTexture *texture() const noexcept
    {
        return straightRgba_;
    }

    [[nodiscard]] float imageOpacity() const noexcept { return imageOpacity_; }

    [[nodiscard]] float repeat() const noexcept { return repeat_; }

    [[nodiscard]] float linearBlending() const noexcept
    {
        return linearBlending_;
    }

    [[nodiscard]] const std::array<float, 4> &background() const noexcept
    {
        return background_;
    }

    [[nodiscard]] const std::array<float, 4> &destination() const noexcept
    {
        return destination_;
    }

private:
    TerminalStraightRgbaTexture *straightRgba_ = nullptr;
    std::array<float, 4> background_{};
    std::array<float, 4> destination_{};
    float imageOpacity_ = 1.0F;
    float repeat_ = 0.0F;
    float linearBlending_ = 0.0F;
    mutable bool uniformsDirty_ = true;
};

namespace {

TerminalBackdropQsgShader::TerminalBackdropQsgShader()
{
    initializeTerminalBackdropShaderResources();
    setShaderFileName(
        VertexStage,
        QStringLiteral(":/ghostty-qt/shaders/terminal_backdrop.vert.qsb"));
    setShaderFileName(
        FragmentStage,
        QStringLiteral(":/ghostty-qt/shaders/terminal_backdrop.frag.qsb"));
}

bool TerminalBackdropQsgShader::updateUniformData(RenderState &state,
                                                  QSGMaterial *newMaterial,
                                                  QSGMaterial *oldMaterial)
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
        writeUniform(buffer, inheritedOpacityOffset, state.opacity());
        changed = true;
    }

    auto *const material =
        static_cast<TerminalBackdropQsgMaterial *>(newMaterial);
    const bool uniformsDirty = material->takeUniformsDirty();
    if (oldMaterial != newMaterial || uniformsDirty) {
        writeUniform(buffer, imageOpacityOffset, material->imageOpacity());
        writeUniform(buffer, repeatOffset, material->repeat());
        writeUniform(buffer, linearBlendingOffset, material->linearBlending());
        writeUniform(buffer, backgroundOffset, material->background());
        writeUniform(buffer, destinationOffset, material->destination());
        changed = true;
    }
    return changed;
}

void TerminalBackdropQsgShader::updateSampledImage(RenderState &state,
                                                   int binding,
                                                   QSGTexture **texture,
                                                   QSGMaterial *newMaterial,
                                                   QSGMaterial *)
{
    auto *const material =
        static_cast<TerminalBackdropQsgMaterial *>(newMaterial);
    TerminalStraightRgbaTexture *const packed =
        binding == 1 ? material->texture() : nullptr;
    QSGTexture *const selected =
        packed != nullptr ? packed->sampledTexture() : nullptr;
    Q_ASSERT(selected != nullptr);
    if (selected == nullptr) return;

    *texture = selected;
    (void)packed->commitTextureOperations(state.rhi(),
                                          state.resourceUpdateBatch());
}

} // namespace

TerminalBackdropQsgNode::TerminalBackdropQsgNode()
    : geometry_(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4))
    , material_(new TerminalBackdropQsgMaterial)
{
    geometry_->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    geometry_->setVertexDataPattern(QSGGeometry::DynamicPattern);
    geometry_->setVertexCount(0);
    setGeometry(geometry_);
    setMaterial(material_);
    setFlag(QSGNode::OwnsGeometry);
    setFlag(QSGNode::OwnsMaterial);
}

TerminalBackdropQsgNode::~TerminalBackdropQsgNode() = default;

bool TerminalBackdropQsgNode::update(
    QQuickWindow *window, const QImage &straightRgba, quint64 assetSerial,
    const QRectF &viewport, const QColor &background, double imageOpacity,
    bool repeat, const QRectF &destination, TerminalAlphaBlending alphaBlending)
{
    constexpr double floatMaximum =
        static_cast<double>(std::numeric_limits<float>::max());
    if (window == nullptr || assetSerial == 0
        || !validStraightRgba(straightRgba) || !finiteRect(viewport)
        || !finiteRect(destination) || !background.isValid()
        || !std::isfinite(imageOpacity) || imageOpacity < -floatMaximum
        || imageOpacity > floatMaximum) {
        clear();
        return false;
    }

    if ((textureWindow_ != window || assetSerial_ != assetSerial
         || !straightRgbaTexture_)
        && !replaceTexture(window, straightRgba, assetSerial)) {
        clear();
        return false;
    }

    if (geometry_->vertexCount() != 4 || viewport_ != viewport) {
        geometry_->setVertexCount(4);
        QSGGeometry::updateRectGeometry(geometry_, viewport);
        viewport_ = viewport;
        markDirty(QSGNode::DirtyGeometry);
    }

    const bool materialChanged =
        material_->setComposition(background, static_cast<float>(imageOpacity),
                                  repeat, destination, alphaBlending);
    if (materialChanged) markDirty(QSGNode::DirtyMaterial);
    return true;
}

void TerminalBackdropQsgNode::clear()
{
    const bool hadTexture = straightRgbaTexture_ != nullptr;
    if (material_->setTexture(nullptr)) {
        markDirty(QSGNode::DirtyMaterial);
    }
    if (straightRgbaTexture_) {
        retiredTextureUploadCount_ += straightRgbaTexture_->uploadCount();
    }
    straightRgbaTexture_.reset();
    textureWindow_ = nullptr;
    assetSerial_ = 0;
    viewport_ = {};
    if (geometry_->vertexCount() != 0) {
        geometry_->setVertexCount(0);
        markDirty(QSGNode::DirtyGeometry);
    } else if (hadTexture) {
        markDirty(QSGNode::DirtyMaterial);
    }
}

bool TerminalBackdropQsgNode::isDrawable() const noexcept
{
    return geometry_->vertexCount() == 4 && straightRgbaTexture_;
}

quint64 TerminalBackdropQsgNode::assetSerial() const noexcept
{
    return assetSerial_;
}

quint64 TerminalBackdropQsgNode::textureGeneration() const noexcept
{
    return textureGeneration_;
}

quint64 TerminalBackdropQsgNode::textureUploadCount() const noexcept
{
    return retiredTextureUploadCount_
        + (straightRgbaTexture_ ? straightRgbaTexture_->uploadCount() : 0);
}

quint64 TerminalBackdropQsgNode::textureCount() const noexcept
{
    return straightRgbaTexture_ ? 1 : 0;
}

quint64 TerminalBackdropQsgNode::textureBytes() const noexcept
{
    return straightRgbaTexture_ ? straightRgbaTexture_->logicalBytes() : 0;
}

bool TerminalBackdropQsgNode::replaceTexture(QQuickWindow *window,
                                             const QImage &straightRgba,
                                             quint64 assetSerial)
{
    auto next = TerminalStraightRgbaTexture::create(window, straightRgba);
    if (!next) return false;

    if (straightRgbaTexture_) {
        retiredTextureUploadCount_ += straightRgbaTexture_->uploadCount();
    }
    straightRgbaTexture_ = std::move(next);
    textureWindow_ = window;
    assetSerial_ = assetSerial;
    ++textureGeneration_;
    if (material_->setTexture(straightRgbaTexture_.get())) {
        markDirty(QSGNode::DirtyMaterial);
    }
    return true;
}

TerminalBackdropSceneNode::TerminalBackdropSceneNode()
    : hardwareBackdrop_(new TerminalBackdropQsgNode)
    , cpuImage_(new QSGSimpleTextureNode)
{
    appendChildNode(hardwareBackdrop_);
    for (QSGSimpleRectNode *&node : baseBackgrounds_) {
        node = new QSGSimpleRectNode;
        appendChildNode(node);
    }
    cpuImage_->setOwnsTexture(false);
}

TerminalBackdropSceneNode::~TerminalBackdropSceneNode()
{
    if (cpuImage_ != nullptr) {
        if (cpuImage_->parent() != nullptr) {
            cpuImage_->parent()->removeChildNode(cpuImage_);
        }
        delete cpuImage_;
        cpuImage_ = nullptr;
    }
    texture_.reset();
}

void TerminalBackdropSceneNode::clearCpuTexture()
{
    if (texture_ || cpuImage_->parent() != nullptr) {
        cpuImage_->setRect({});
        if (cpuImage_->parent() != nullptr) {
            cpuImage_->parent()->removeChildNode(cpuImage_);
        }
        delete cpuImage_;
        cpuImage_ = new QSGSimpleTextureNode;
        cpuImage_->setOwnsTexture(false);
    }
    texture_.reset();
    textureKey_.reset();
    textureRepeat_ = false;
}

void TerminalBackdropSceneNode::setBase(std::size_t index,
                                        const QRectF &rect,
                                        const QColor &color)
{
    QSGSimpleRectNode *const node = baseBackgrounds_[index];
    const QRectF effective = rect.isEmpty() ? QRectF{} : rect;
    if (node->rect() != effective) node->setRect(effective);
    const QColor effectiveColor = effective.isEmpty()
        ? QColor(Qt::transparent)
        : terminalRenderingColor(color, alphaBlending_);
    if (node->color() != effectiveColor) node->setColor(effectiveColor);
}

void TerminalBackdropSceneNode::clearBases(const QColor &color)
{
    for (std::size_t index = 0; index < baseBackgrounds_.size(); ++index) {
        setBase(index, {}, color);
    }
}

void TerminalBackdropSceneNode::setSolidBase(const QRectF &viewport,
                                              const QColor &color)
{
    setBase(0, viewport, color);
    for (std::size_t index = 1; index < baseBackgrounds_.size(); ++index) {
        setBase(index, {}, color);
    }
}

void TerminalBackdropSceneNode::update(
    QQuickWindow *window, const QRectF &viewport, const QColor &background,
    const std::shared_ptr<const TerminalBackgroundImageAsset> &asset,
    const TerminalBackgroundImageOptions &options, qreal devicePixelRatio,
    bool useCustomMaterial, TerminalAlphaBlending alphaBlending)
{
    alphaBlending_ = alphaBlending;
    const auto useSolidFallback = [&] {
        hardwareBackdrop_->clear();
        clearCpuTexture();
        setSolidBase(viewport, background);
        imageRect_ = {};
        sourceRect_ = {};
    };

    if (window == nullptr || asset == nullptr || asset->straightRgba.isNull()) {
        failedMaterialKey_.reset();
        useSolidFallback();
        return;
    }

    const QRectF target = terminalBackgroundImagePlacement(
        viewport, asset->straightRgba.size(), devicePixelRatio, options.fit,
        options.position);
    if (target.isEmpty()) {
        failedMaterialKey_.reset();
        useSolidFallback();
        return;
    }

    if (useCustomMaterial) {
        const MaterialFailureKey materialKey{
            .assetSerial = asset->serial,
            .window = window,
        };
        if (failedMaterialKey_ != materialKey) {
            if (hardwareBackdrop_->update(window, asset->straightRgba,
                                          asset->serial, viewport, background,
                                          options.opacity, options.repeat,
                                          target, alphaBlending)) {
                failedMaterialKey_.reset();
                clearCpuTexture();
                clearBases(background);
                imageRect_ = options.repeat
                    ? viewport
                    : target.intersected(viewport);
                sourceRect_ = mappedSourceRect(imageRect_, target,
                                               asset->straightRgba.size());
                return;
            }
            // Avoid retrying a failed GPU upload on every cursor blink.
            // A different image or scene-graph window gets a fresh try.
            failedMaterialKey_ = materialKey;
        }
    } else {
        failedMaterialKey_.reset();
    }
    hardwareBackdrop_->clear();

    const CpuTextureKey textureKey{
        .assetSerial = asset->serial,
        .window = window,
        .background = background.rgba(),
        .imageOpacityBits = std::bit_cast<quint64>(options.opacity),
        .alphaBlending = alphaBlending,
    };
    if (!texture_ && failedTextureKey_ == textureKey) {
        setSolidBase(viewport, background);
        imageRect_ = {};
        sourceRect_ = {};
        return;
    }

    if (textureKey_ != textureKey) {
        clearCpuTexture();
        QColor opaqueColor = background;
        opaqueColor.setAlpha(255);
        const QImage composited = linearizedPremultipliedImage(
            terminalCompositedBackgroundImage(
                *asset, opaqueColor, static_cast<quint8>(background.alpha()),
                options.opacity),
            alphaBlending);
        texture_.reset(window->createTextureFromImage(
            composited, QQuickWindow::TextureHasAlphaChannel));
        if (!texture_) {
            failedTextureKey_ = textureKey;
            setSolidBase(viewport, background);
            imageRect_ = {};
            sourceRect_ = {};
            return;
        }

        failedTextureKey_.reset();
        textureKey_ = textureKey;
        texture_->setFiltering(QSGTexture::Linear);
        cpuImage_->setTexture(texture_.get());
        appendChildNode(cpuImage_);
    }

    if (textureRepeat_ != options.repeat) {
        texture_->setHorizontalWrapMode(
            options.repeat ? QSGTexture::Repeat
                           : QSGTexture::ClampToEdge);
        texture_->setVerticalWrapMode(
            options.repeat ? QSGTexture::Repeat
                           : QSGTexture::ClampToEdge);
        textureRepeat_ = options.repeat;
    }

    if (options.repeat) {
        clearBases(background);
        const QRectF nextSource =
            mappedSourceRect(viewport, target, asset->straightRgba.size());
        if (cpuImage_->rect() != viewport) cpuImage_->setRect(viewport);
        if (cpuImage_->sourceRect() != nextSource) {
            cpuImage_->setSourceRect(nextSource);
        }
        imageRect_ = viewport;
        sourceRect_ = nextSource;
        return;
    }

    const QRectF visible = target.intersected(viewport);
    if (visible.isEmpty()) {
        clearCpuTexture();
        setSolidBase(viewport, background);
        imageRect_ = {};
        sourceRect_ = {};
        return;
    }
    const QRectF nextSource =
        mappedSourceRect(visible, target, asset->straightRgba.size());
    if (cpuImage_->rect() != visible) cpuImage_->setRect(visible);
    if (cpuImage_->sourceRect() != nextSource) {
        cpuImage_->setSourceRect(nextSource);
    }
    imageRect_ = visible;
    sourceRect_ = nextSource;

    // The software fallback is already composited with the base. Keep the
    // surrounding rectangles non-overlapping so global alpha is applied once.
    setBase(0,
            QRectF(viewport.left(), viewport.top(), viewport.width(),
                   visible.top() - viewport.top()),
            background);
    setBase(1,
            QRectF(viewport.left(), visible.bottom(), viewport.width(),
                   viewport.bottom() - visible.bottom()),
            background);
    setBase(2,
            QRectF(viewport.left(), visible.top(),
                   visible.left() - viewport.left(), visible.height()),
            background);
    setBase(3,
            QRectF(visible.right(), visible.top(),
                   viewport.right() - visible.right(), visible.height()),
            background);
}

quint64 TerminalBackdropSceneNode::assetSerial() const noexcept
{
    return hardwareBackdrop_->isDrawable()
        ? hardwareBackdrop_->assetSerial()
        : textureKey_.transform(
              [](const CpuTextureKey &key) { return key.assetSerial; })
              .value_or(0);
}

const QRectF &TerminalBackdropSceneNode::imageRect() const noexcept
{
    return imageRect_;
}

const QRectF &TerminalBackdropSceneNode::sourceRect() const noexcept
{
    return sourceRect_;
}

std::array<QRectF, 4>
TerminalBackdropSceneNode::baseRects() const noexcept
{
    std::array<QRectF, 4> result;
    std::ranges::transform(
        baseBackgrounds_, result.begin(),
        [](const QSGSimpleRectNode *node) { return node->rect(); });
    return result;
}
