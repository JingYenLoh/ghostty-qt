#include "terminal_glyph_batch.h"

#include <QByteArray>
#include <QMatrix4x4>
#include <QSGGeometry>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <ranges>
#include <utility>

static void initializeTerminalGlyphShaderResources()
{
    static const bool initialized = [] {
        Q_INIT_RESOURCE(ghostty_qt_terminal_glyph_shaders);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace {

constexpr qsizetype verticesPerGlyph = 4;
constexpr qsizetype indicesPerGlyph = 6;
constexpr qsizetype maximumGlyphCount =
    (static_cast<qsizetype>(std::numeric_limits<quint16>::max()) + 1)
    / verticesPerGlyph;
constexpr qsizetype matrixOffset = 0;
constexpr qsizetype matrixSize = sizeof(float) * 16;
constexpr qsizetype inheritedOpacityOffset = matrixOffset + matrixSize;
constexpr qsizetype uniformBufferSize = inheritedOpacityOffset + sizeof(float);

static_assert(matrixSize == 64);
static_assert(uniformBufferSize == 68);
static_assert(maximumGlyphCount * verticesPerGlyph - 1
              == std::numeric_limits<quint16>::max());

struct GlyphVertex {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    uchar red = 0;
    uchar green = 0;
    uchar blue = 0;
    uchar alpha = 0;
};

static_assert(sizeof(GlyphVertex) == 20);

const std::array<QSGGeometry::Attribute, 3> glyphAttributes{
    QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true),
    QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType),
    QSGGeometry::Attribute::createWithAttributeType(
        2, 4, QSGGeometry::UnsignedByteType, QSGGeometry::ColorAttribute),
};

const QSGGeometry::AttributeSet glyphAttributeSet{
    static_cast<int>(glyphAttributes.size()),
    static_cast<int>(sizeof(GlyphVertex)), glyphAttributes.data()};

[[nodiscard]] qsizetype grownCapacity(qsizetype current,
                                      qsizetype required) noexcept
{
    if (current >= required) return current;
    const qsizetype growth = current > maximumGlyphCount - current / 2
        ? maximumGlyphCount
        : current + current / 2;
    return std::max(required, std::max<qsizetype>(growth, 64));
}

[[nodiscard]] bool finitePositiveRect(const TerminalGlyphRect &rect) noexcept
{
    return std::isfinite(rect.left) && std::isfinite(rect.top)
        && std::isfinite(rect.right) && std::isfinite(rect.bottom)
        && rect.right > rect.left && rect.bottom > rect.top;
}

[[nodiscard]] bool validSourceRect(const TerminalGlyphRect &rect) noexcept
{
    constexpr float tolerance = std::numeric_limits<float>::epsilon() * 16.0F;
    return finitePositiveRect(rect) && rect.left >= -tolerance
        && rect.top >= -tolerance && rect.right <= 1.0F + tolerance
        && rect.bottom <= 1.0F + tolerance;
}

[[nodiscard]] bool validGlyph(const TerminalGlyphQuad &glyph) noexcept
{
    return finitePositiveRect(glyph.destination)
        && validSourceRect(glyph.normalizedSource) && glyph.color.isValid();
}

[[nodiscard]] bool countFitsGeometry(qsizetype glyphCount) noexcept
{
    return glyphCount >= 0 && glyphCount <= maximumGlyphCount;
}

void setVertex(GlyphVertex &vertex, float x, float y, float u, float v,
               uchar red, uchar green, uchar blue, uchar alpha) noexcept
{
    vertex = {
        .x = x,
        .y = y,
        .u = u,
        .v = v,
        .red = red,
        .green = green,
        .blue = blue,
        .alpha = alpha,
    };
}

class TerminalGlyphBatchShader final : public QSGMaterialShader {
public:
    TerminalGlyphBatchShader()
    {
        initializeTerminalGlyphShaderResources();
        setShaderFileName(
            VertexStage,
            QStringLiteral(":/ghostty-qt/shaders/terminal_glyph.vert.qsb"));
        setShaderFileName(
            FragmentStage,
            QStringLiteral(":/ghostty-qt/shaders/terminal_glyph.frag.qsb"));
    }

    bool updateUniformData(RenderState &state, QSGMaterial *,
                           QSGMaterial *oldMaterial) override
    {
        QByteArray &buffer = *state.uniformData();
        Q_ASSERT(buffer.size() >= uniformBufferSize);
        bool changed = false;
        if (oldMaterial == nullptr || state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
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

    void updateSampledImage(RenderState &, int binding, QSGTexture **texture,
                            QSGMaterial *newMaterial, QSGMaterial *) override;
};

} // namespace

namespace {

class TerminalGlyphBatchMaterial final : public QSGMaterial {
public:
    TerminalGlyphBatchMaterial() { setFlag(QSGMaterial::Blending); }

    QSGMaterialType *type() const override
    {
        static QSGMaterialType materialType;
        return &materialType;
    }

    QSGMaterialShader *
    createShader(QSGRendererInterface::RenderMode) const override
    {
        return new TerminalGlyphBatchShader;
    }

    int compare(const QSGMaterial *other) const override
    {
        const auto *right =
            static_cast<const TerminalGlyphBatchMaterial *>(other);
        if (texture_ == right->texture_) return 0;
        return std::less<QSGTexture *>{}(texture_, right->texture_) ? -1 : 1;
    }

    [[nodiscard]] QSGTexture *texture() const noexcept { return texture_; }

    [[nodiscard]] bool setTexture(QSGTexture *texture) noexcept
    {
        if (texture_ == texture) return false;
        texture_ = texture;
        return true;
    }

private:
    QSGTexture *texture_ = nullptr;
};

void TerminalGlyphBatchShader::updateSampledImage(RenderState &state,
                                                  int binding,
                                                  QSGTexture **texture,
                                                  QSGMaterial *newMaterial,
                                                  QSGMaterial *)
{
    auto *const material =
        static_cast<TerminalGlyphBatchMaterial *>(newMaterial);
    QSGTexture *const selected = binding == 1 ? material->texture() : nullptr;
    Q_ASSERT(selected != nullptr);
    if (selected == nullptr) return;
    *texture = selected;
    selected->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
}

} // namespace

TerminalGlyphBatch::TerminalGlyphBatch()
    : geometry_(new QSGGeometry(glyphAttributeSet, 0, 0,
                                QSGGeometry::UnsignedShortType))
    , material_(new TerminalGlyphBatchMaterial)
{
    geometry_->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry_->setVertexDataPattern(QSGGeometry::DynamicPattern);
    geometry_->setIndexDataPattern(QSGGeometry::DynamicPattern);
    setGeometry(geometry_);
    setFlag(QSGNode::OwnsGeometry);
    setMaterial(material_);
    setFlag(QSGNode::OwnsMaterial);
}

TerminalGlyphBatch::~TerminalGlyphBatch() = default;

QVector<TerminalGlyphQuad> &TerminalGlyphBatch::beginUpdate() noexcept
{
    pending_.clear();
    return pending_;
}

TerminalGlyphBatchCommitResult
TerminalGlyphBatch::commit(QSGTexture *atlasTexture,
                           TerminalAlphaBlending alphaBlending)
{
    const TerminalGlyphBatchCommitResult nextResult =
        requestedResult(atlasTexture);
    QSGTexture *const nextTexture =
        nextResult == TerminalGlyphBatchCommitResult::Batched ? atlasTexture
                                                              : nullptr;
    const bool contentChanged = pending_ != committed_;
    const bool colorSpaceChanged = committedAlphaBlending_ != alphaBlending;
    const bool textureChanged = committedTexture_ != nextTexture;
    const bool resultChanged = result_ != nextResult;
    if (hasCommit_ && !contentChanged && !colorSpaceChanged && !textureChanged
        && !resultChanged) {
        return result_;
    }

    const bool wasEmpty = geometry_->vertexCount() == 0;
    result_ = nextResult;
    committedAlphaBlending_ = alphaBlending;
    committedTexture_ = nextTexture;

    auto *const glyphMaterial =
        static_cast<TerminalGlyphBatchMaterial *>(material_);
    if (glyphMaterial->setTexture(nextTexture)) {
        ++materialAssignmentCount_;
        markDirty(QSGNode::DirtyMaterial);
    }

    if (nextResult == TerminalGlyphBatchCommitResult::Batched) {
        allocateFor(pending_.size());
        if (contentChanged || colorSpaceChanged || resultChanged) {
            writeGeometry();
        }
        if (wasEmpty && geometry_->vertexCount() > 0) {
            // The RHI renderer may detach zero-sized alpha geometry from its
            // batch. Material invalidation makes it discoverable again.
            markDirty(QSGNode::DirtyMaterial);
        }
    } else {
        hideGeometry();
        if (nextResult == TerminalGlyphBatchCommitResult::Fallback) {
            ++fallbackCommitCount_;
        }
    }

    committed_.swap(pending_);
    hasCommit_ = true;
    ++commitGeneration_;
    return result_;
}

TerminalGlyphBatchCommitResult TerminalGlyphBatch::result() const noexcept
{
    return result_;
}

qsizetype TerminalGlyphBatch::size() const noexcept
{
    return committed_.size();
}

qsizetype TerminalGlyphBatch::capacity() const noexcept
{
    return capacity_;
}

quint64 TerminalGlyphBatch::allocationGeneration() const noexcept
{
    return allocationGeneration_;
}

quint64 TerminalGlyphBatch::commitGeneration() const noexcept
{
    return commitGeneration_;
}

quint64 TerminalGlyphBatch::geometryWriteCount() const noexcept
{
    return geometryWriteCount_;
}

quint64 TerminalGlyphBatch::materialAssignmentCount() const noexcept
{
    return materialAssignmentCount_;
}

quint64 TerminalGlyphBatch::fallbackCommitCount() const noexcept
{
    return fallbackCommitCount_;
}

TerminalGlyphBatchProbeSnapshot TerminalGlyphBatch::probe() const noexcept
{
    return {
        .result = result_,
        .glyphCount = committed_.size(),
        .vertexCount = geometry_->vertexCount(),
        .indexCount = geometry_->indexCount(),
        .capacity = capacity_,
        .allocationGeneration = allocationGeneration_,
        .commitGeneration = commitGeneration_,
        .geometryWriteCount = geometryWriteCount_,
        .materialAssignmentCount = materialAssignmentCount_,
        .fallbackCommitCount = fallbackCommitCount_,
    };
}

TerminalGlyphBatchCommitResult
TerminalGlyphBatch::requestedResult(QSGTexture *atlasTexture) const noexcept
{
    if (pending_.isEmpty()) return TerminalGlyphBatchCommitResult::Empty;
    if (atlasTexture == nullptr || atlasTexture->textureSize().isEmpty()
        || !atlasTexture->hasAlphaChannel()
        || !countFitsGeometry(pending_.size())) {
        return TerminalGlyphBatchCommitResult::Fallback;
    }
    return std::ranges::all_of(pending_, validGlyph)
        ? TerminalGlyphBatchCommitResult::Batched
        : TerminalGlyphBatchCommitResult::Fallback;
}

void TerminalGlyphBatch::allocateFor(qsizetype glyphCount)
{
    if (glyphCount <= capacity_) {
        geometry_->setVertexCount(
            static_cast<int>(glyphCount * verticesPerGlyph));
        geometry_->setIndexCount(
            static_cast<int>(glyphCount * indicesPerGlyph));
        return;
    }

    capacity_ = grownCapacity(capacity_, glyphCount);
    geometry_->allocate(static_cast<int>(capacity_ * verticesPerGlyph),
                        static_cast<int>(capacity_ * indicesPerGlyph));
    auto *const indices = geometry_->indexDataAsUShort();
    for (qsizetype index = 0; index < capacity_; ++index) {
        const quint16 vertex = static_cast<quint16>(index * verticesPerGlyph);
        const qsizetype output = index * indicesPerGlyph;
        indices[output] = vertex;
        indices[output + 1] = vertex + 1;
        indices[output + 2] = vertex + 2;
        indices[output + 3] = vertex + 2;
        indices[output + 4] = vertex + 1;
        indices[output + 5] = vertex + 3;
    }
    geometry_->setVertexCount(static_cast<int>(glyphCount * verticesPerGlyph));
    geometry_->setIndexCount(static_cast<int>(glyphCount * indicesPerGlyph));
    geometry_->markIndexDataDirty();
    ++allocationGeneration_;
}

void TerminalGlyphBatch::writeGeometry()
{
    auto *vertices = static_cast<GlyphVertex *>(geometry_->vertexData());
    QColor previousLogicalColor;
    uchar red = 0;
    uchar green = 0;
    uchar blue = 0;
    uchar opacity = 0;
    bool hasPreviousColor = false;
    for (const TerminalGlyphQuad &glyph : pending_) {
        const TerminalGlyphRect &destination = glyph.destination;
        const TerminalGlyphRect &source = glyph.normalizedSource;
        if (!hasPreviousColor || previousLogicalColor != glyph.color) {
            previousLogicalColor = glyph.color;
            const QColor color =
                terminalRenderingColor(glyph.color, committedAlphaBlending_)
                    .toRgb();
            const int alpha = color.alpha();
            const auto premultiply = [alpha](int component) {
                return static_cast<uchar>((component * alpha + 127) / 255);
            };
            red = premultiply(color.red());
            green = premultiply(color.green());
            blue = premultiply(color.blue());
            opacity = static_cast<uchar>(alpha);
            hasPreviousColor = true;
        }

        setVertex(vertices[0], destination.left, destination.top, source.left,
                  source.top, red, green, blue, opacity);
        setVertex(vertices[1], destination.right, destination.top, source.right,
                  source.top, red, green, blue, opacity);
        setVertex(vertices[2], destination.left, destination.bottom,
                  source.left, source.bottom, red, green, blue, opacity);
        setVertex(vertices[3], destination.right, destination.bottom,
                  source.right, source.bottom, red, green, blue, opacity);
        vertices += verticesPerGlyph;
    }
    geometry_->markVertexDataDirty();
    markDirty(QSGNode::DirtyGeometry);
    ++geometryWriteCount_;
}

void TerminalGlyphBatch::hideGeometry()
{
    if (geometry_->vertexCount() == 0 && geometry_->indexCount() == 0) return;
    geometry_->setVertexCount(0);
    geometry_->setIndexCount(0);
    markDirty(QSGNode::DirtyGeometry);
}
