#include "terminal_custom_shader_qsg.h"

#include "terminal_custom_shader_compiler.h"

#include <QByteArray>
#include <QHash>
#include <QMatrix4x4>
#include <QMutex>
#include <QMutexLocker>
#include <QResource>
#include <QSGGeometry>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QSGTextureProvider>
#include <rhi/qshader.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

static void initializeTerminalCustomShaderResources()
{
    static const bool initialized = [] {
        Q_INIT_RESOURCE(ghostty_qt_terminal_custom_shader_shaders);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace {

struct ProgramIdentityKey {
    QString qsbPath;
    QByteArray cacheKey;

    bool operator==(const ProgramIdentityKey &) const = default;
};

size_t qHash(const ProgramIdentityKey &key, size_t seed = 0) noexcept
{
    return qHashMulti(seed, key.qsbPath, key.cacheKey);
}

struct ProgramIdentityRegistry {
    QMutex mutex;
    QHash<ProgramIdentityKey, QSGMaterialType *> identities;
    std::vector<std::unique_ptr<QSGMaterialType>> storage;
};

ProgramIdentityRegistry &programIdentityRegistry()
{
    // QSG renderer contexts cache shaders by QSGMaterialType pointer. Retain
    // the tiny identity tokens for process lifetime so a reload can never
    // leave a dangling or accidentally reused cache key.
    static auto *const registry = new ProgramIdentityRegistry;
    return *registry;
}

QSGMaterialType *materialTypeFor(const QString &qsbPath,
                                 const QByteArray &cacheKey)
{
    ProgramIdentityRegistry &registry = programIdentityRegistry();
    const ProgramIdentityKey key{
        .qsbPath = qsbPath,
        .cacheKey = cacheKey.isEmpty() ? qsbPath.toUtf8() : cacheKey,
    };
    QMutexLocker lock(&registry.mutex);
    if (auto found = registry.identities.constFind(key);
        found != registry.identities.cend()) {
        return found.value();
    }

    auto identity = std::make_unique<QSGMaterialType>();
    QSGMaterialType *const result = identity.get();
    registry.storage.push_back(std::move(identity));
    registry.identities.insert(key, result);
    return result;
}

[[nodiscard]] bool finiteRect(const QRectF &rect) noexcept
{
    return std::isfinite(rect.x()) && std::isfinite(rect.y())
        && std::isfinite(rect.width()) && std::isfinite(rect.height())
        && rect.width() > 0.0 && rect.height() > 0.0;
}

template <typename Value>
void writeUniform(QByteArray &buffer, std::size_t offset, const Value &value)
{
    static_assert(std::is_trivially_copyable_v<Value>);
    Q_ASSERT(offset + sizeof(Value) <= static_cast<std::size_t>(buffer.size()));
    std::memcpy(buffer.data() + static_cast<qsizetype>(offset), &value,
                sizeof(Value));
}

class TerminalCustomShaderQsgShader final : public QSGMaterialShader {
public:
    explicit TerminalCustomShaderQsgShader(const QShader &fragmentShader)
    {
        initializeTerminalCustomShaderResources();
        setShaderFileName(
            VertexStage,
            QStringLiteral(
                ":/ghostty-qt/shaders/terminal_custom_shader.vert.qsb"));
        setShader(FragmentStage, fragmentShader);
    }

    bool updateUniformData(RenderState &state, QSGMaterial *newMaterial,
                           QSGMaterial *oldMaterial) override;
    void updateSampledImage(RenderState &state, int binding,
                            QSGTexture **texture, QSGMaterial *newMaterial,
                            QSGMaterial *oldMaterial) override;
};

} // namespace

class TerminalCustomShaderQsgMaterial final : public QSGMaterial {
public:
    TerminalCustomShaderQsgMaterial(
        QSGTexture *source,
        std::shared_ptr<const TerminalCustomShaderProgram> program,
        TerminalCustomShaderUniformSnapshot uniforms)
        : source_(source)
        , program_(std::move(program))
        , uniforms_(std::move(uniforms))
    {
        setFlag(QSGMaterial::Blending);
    }

    QSGMaterialType *type() const override { return program_->materialType_; }

    QSGMaterialShader *
    createShader(QSGRendererInterface::RenderMode) const override
    {
        return new TerminalCustomShaderQsgShader(program_->shader());
    }

    int compare(const QSGMaterial *other) const override
    {
        const auto *right =
            static_cast<const TerminalCustomShaderQsgMaterial *>(other);
        const qint64 leftTexture =
            source_ != nullptr ? source_->comparisonKey() : 0;
        const qint64 rightTexture =
            right->source_ != nullptr ? right->source_->comparisonKey() : 0;
        if (leftTexture != rightTexture) {
            return leftTexture < rightTexture ? -1 : 1;
        }
        const std::less<const TerminalCustomShaderUniforms *> less;
        if (uniforms_.get() != right->uniforms_.get()) {
            return less(uniforms_.get(), right->uniforms_.get()) ? -1 : 1;
        }
        return 0;
    }

    [[nodiscard]] bool setState(QSGTexture *source,
                                TerminalCustomShaderUniformSnapshot uniforms)
    {
        bool changed = false;
        if (source_ != source) {
            source_ = source;
            changed = true;
        }
        if (uniforms_ != uniforms) {
            uniforms_ = std::move(uniforms);
            uniformsDirty_ = true;
            changed = true;
        }
        return changed;
    }

    [[nodiscard]] QSGTexture *source() const noexcept { return source_; }

    [[nodiscard]] const TerminalCustomShaderUniformSnapshot &
    uniforms() const noexcept
    {
        return uniforms_;
    }

    [[nodiscard]] bool takeUniformsDirty() const noexcept
    {
        return std::exchange(uniformsDirty_, false);
    }

    [[nodiscard]] const std::shared_ptr<const TerminalCustomShaderProgram> &
    program() const noexcept
    {
        return program_;
    }

private:
    QSGTexture *source_ = nullptr;
    std::shared_ptr<const TerminalCustomShaderProgram> program_;
    TerminalCustomShaderUniformSnapshot uniforms_;
    mutable bool uniformsDirty_ = true;
};

namespace {

bool TerminalCustomShaderQsgShader::updateUniformData(RenderState &state,
                                                      QSGMaterial *newMaterial,
                                                      QSGMaterial *oldMaterial)
{
    QByteArray &buffer = *state.uniformData();
    if (buffer.size()
        < static_cast<qsizetype>(TerminalCustomShaderUniformLayout::size)) {
        qWarning("Custom shader has an incompatible uniform block size.");
        return false;
    }

    bool changed = false;
    if (oldMaterial == nullptr || state.isMatrixDirty()) {
        const QMatrix4x4 matrix = state.combinedMatrix();
        static_assert(sizeof(float) * 16 == 64);
        std::memcpy(buffer.data()
                        + static_cast<qsizetype>(
                            TerminalCustomShaderUniformLayout::qtMatrix),
                    matrix.constData(), sizeof(float) * 16);
        changed = true;
    }
    if (oldMaterial == nullptr || state.isOpacityDirty()) {
        writeUniform(buffer, TerminalCustomShaderUniformLayout::qtOpacity,
                     state.opacity());
        changed = true;
    }

    auto *const material =
        static_cast<TerminalCustomShaderQsgMaterial *>(newMaterial);
    const bool uniformsDirty = material->takeUniformsDirty();
    if (oldMaterial != newMaterial || uniformsDirty
        || state.dirtyStates().testFlag(
            QSGMaterialShader::RenderState::DirtyCachedMaterialData)) {
        const TerminalCustomShaderUniformSnapshot &uniforms =
            material->uniforms();
        Q_ASSERT(uniforms != nullptr);
        if (uniforms != nullptr) {
            constexpr std::size_t offset =
                TerminalCustomShaderUniformLayout::ghostty;
            constexpr std::size_t byteCount =
                TerminalCustomShaderUniformLayout::size - offset;
            const auto *const source =
                reinterpret_cast<const std::byte *>(uniforms.get());
            std::memcpy(buffer.data() + static_cast<qsizetype>(offset),
                        source + offset, byteCount);
            changed = true;
        }
    }
    return changed;
}

void TerminalCustomShaderQsgShader::updateSampledImage(RenderState &state,
                                                       int binding,
                                                       QSGTexture **texture,
                                                       QSGMaterial *newMaterial,
                                                       QSGMaterial *)
{
    auto *const material =
        static_cast<TerminalCustomShaderQsgMaterial *>(newMaterial);
    QSGTexture *const source = binding == 1 ? material->source() : nullptr;
    Q_ASSERT(source != nullptr);
    if (source == nullptr) return;

    source->setFiltering(QSGTexture::Linear);
    source->setMipmapFiltering(QSGTexture::None);
    source->setHorizontalWrapMode(QSGTexture::ClampToEdge);
    source->setVerticalWrapMode(QSGTexture::ClampToEdge);
    *texture = source;
    source->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
}

} // namespace

struct TerminalCustomShaderProgram::Data {
    explicit Data(const QByteArray &serializedShader)
        : shader(QShader::fromSerialized(serializedShader))
    {}

    QShader shader;
};

TerminalCustomShaderProgram::TerminalCustomShaderProgram(
    QString qsbPath, QByteArray cacheKey, QByteArray serializedShader)
    : qsbPath_(std::move(qsbPath))
    , cacheKey_(std::move(cacheKey))
{
    if (!qsbPath_.isEmpty() && !serializedShader.isEmpty()) {
        data_ = std::make_shared<const Data>(serializedShader);
    }
    if (data_ != nullptr && data_->shader.isValid()
        && data_->shader.stage() == QShader::FragmentStage) {
        materialType_ = materialTypeFor(qsbPath_, cacheKey_);
    } else {
        data_.reset();
    }
}

const QString &TerminalCustomShaderProgram::qsbPath() const noexcept
{
    return qsbPath_;
}

const QByteArray &TerminalCustomShaderProgram::cacheKey() const noexcept
{
    return cacheKey_;
}

const QShader &TerminalCustomShaderProgram::shader() const noexcept
{
    Q_ASSERT(data_ != nullptr);
    return data_->shader;
}

bool TerminalCustomShaderProgram::isValid() const noexcept
{
    return !qsbPath_.isEmpty() && data_ != nullptr && data_->shader.isValid()
        && materialType_ != nullptr;
}

TerminalCustomShaderQsgNode::TerminalCustomShaderQsgNode()
    : geometry_(
          new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4))
{
    geometry_->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    geometry_->setVertexDataPattern(QSGGeometry::DynamicPattern);
    geometry_->setVertexCount(0);
    setGeometry(geometry_);
    setFlag(QSGNode::OwnsGeometry);
    setFlag(QSGNode::OwnsMaterial);
}

TerminalCustomShaderQsgNode::~TerminalCustomShaderQsgNode() = default;

bool TerminalCustomShaderQsgNode::update(
    QSGTexture *source, const QRectF &viewport,
    std::shared_ptr<const TerminalCustomShaderProgram> program,
    TerminalCustomShaderUniformSnapshot uniforms)
{
    if (source == nullptr || source->isAtlasTexture()
        || source->textureSize().isEmpty() || !finiteRect(viewport)
        || program == nullptr || !program->isValid() || uniforms == nullptr) {
        clear();
        return false;
    }

    if (material_ == nullptr || material_->program() != program) {
        auto *const next = new TerminalCustomShaderQsgMaterial(
            source, std::move(program), std::move(uniforms));
        setMaterial(next);
        material_ = next;
        markDirty(QSGNode::DirtyMaterial);
    } else if (material_->setState(source, std::move(uniforms))) {
        markDirty(QSGNode::DirtyMaterial);
    }

    const QRectF textureCoordinates = source->normalizedTextureSubRect();
    if (geometry_->vertexCount() != 4 || viewport_ != viewport
        || textureCoordinates_ != textureCoordinates) {
        geometry_->setVertexCount(4);
        QSGGeometry::updateTexturedRectGeometry(geometry_, viewport,
                                                textureCoordinates);
        viewport_ = viewport;
        textureCoordinates_ = textureCoordinates;
        markDirty(QSGNode::DirtyGeometry);
    }
    return true;
}

void TerminalCustomShaderQsgNode::clear()
{
    if (material_ != nullptr
        && material_->setState(nullptr, material_->uniforms())) {
        markDirty(QSGNode::DirtyMaterial);
    }
    viewport_ = {};
    textureCoordinates_ = {};
    if (geometry_->vertexCount() != 0) {
        geometry_->setVertexCount(0);
        markDirty(QSGNode::DirtyGeometry);
    }
}

bool TerminalCustomShaderQsgNode::isDrawable() const noexcept
{
    return geometry_->vertexCount() == 4 && material_ != nullptr
        && material_->source() != nullptr;
}

TerminalCustomShaderEffect::TerminalCustomShaderEffect(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents);
}

TerminalCustomShaderEffect::~TerminalCustomShaderEffect()
{
    detachFromProvider();
}

QQuickItem *TerminalCustomShaderEffect::source() const noexcept
{
    return source_;
}

void TerminalCustomShaderEffect::setSource(QQuickItem *source)
{
    if (source_ == source) return;
    const bool wasActive = isActive();
    QObject::disconnect(sourceDestroyedConnection_);
    source_ = source;
    if (source_ != nullptr) {
        sourceDestroyedConnection_ =
            connect(source_, &QObject::destroyed, this, [this] {
                const bool activeBeforeDestruction = program_ != nullptr
                    && program_->isValid() && providerInterface() != nullptr;
                source_.clear();
                Q_EMIT sourceChanged();
                updateActive(activeBeforeDestruction);
                update();
            });
    } else {
        sourceDestroyedConnection_ = {};
    }
    Q_EMIT sourceChanged();
    updateActive(wasActive);
    update();
}

QString TerminalCustomShaderEffect::fragmentShaderFileName() const
{
    return fragmentShaderFileName_;
}

void TerminalCustomShaderEffect::setFragmentShaderFileName(const QString &path)
{
    if (fragmentShaderFileName_ == path) return;
    const bool wasActive = isActive();
    fragmentShaderFileName_ = path;
    rebuildProgram();
    Q_EMIT fragmentShaderFileNameChanged();
    updateActive(wasActive);
    update();
}

QByteArray TerminalCustomShaderEffect::fragmentShaderData() const
{
    return fragmentShaderData_;
}

void TerminalCustomShaderEffect::setFragmentShaderData(const QByteArray &data)
{
    if (fragmentShaderData_ == data) return;
    const bool wasActive = isActive();
    fragmentShaderData_ = data;
    rebuildProgram();
    Q_EMIT fragmentShaderDataChanged();
    updateActive(wasActive);
    update();
}

void TerminalCustomShaderEffect::setStage(
    const TerminalCustomShaderStage &stage)
{
    const bool unchanged = fragmentShaderFileName_ == stage.qsbPath
        && fragmentShaderData_ == stage.serializedShader && program_ != nullptr
        && program_->cacheKey() == stage.cacheKey;
    if (unchanged) return;

    const bool wasActive = isActive();
    fragmentShaderFileName_ = stage.qsbPath;
    fragmentShaderData_ = stage.serializedShader;
    program_ = stage.qsbPath.isEmpty()
        ? nullptr
        : std::make_shared<const TerminalCustomShaderProgram>(
              stage.qsbPath, stage.cacheKey, stage.serializedShader);
    Q_EMIT fragmentShaderFileNameChanged();
    Q_EMIT fragmentShaderDataChanged();
    updateActive(wasActive);
    update();
}

void TerminalCustomShaderEffect::rebuildProgram()
{
    program_ =
        fragmentShaderFileName_.isEmpty() || fragmentShaderData_.isEmpty()
        ? nullptr
        : std::make_shared<const TerminalCustomShaderProgram>(
              fragmentShaderFileName_, QByteArray{}, fragmentShaderData_);
}

QObject *TerminalCustomShaderEffect::uniformProvider() const noexcept
{
    return uniformProvider_;
}

void TerminalCustomShaderEffect::setUniformProvider(QObject *provider)
{
    if (uniformProvider_ == provider) return;
    const bool wasActive = isActive();
    detachFromProvider();
    QObject::disconnect(providerDestroyedConnection_);
    uniformProvider_ = provider;
    if (uniformProvider_ != nullptr) {
        providerDestroyedConnection_ =
            connect(uniformProvider_, &QObject::destroyed, this, [this] {
                const bool activeBeforeDestruction = source_ != nullptr
                    && program_ != nullptr && program_->isValid();
                uniformProvider_.clear();
                Q_EMIT uniformProviderChanged();
                updateActive(activeBeforeDestruction);
                update();
            });
    } else {
        providerDestroyedConnection_ = {};
    }
    attachToProvider();
    Q_EMIT uniformProviderChanged();
    updateActive(wasActive);
    update();
}

int TerminalCustomShaderEffect::stageIndex() const noexcept
{
    return stageIndex_;
}

void TerminalCustomShaderEffect::setStageIndex(int stageIndex)
{
    if (stageIndex_ == stageIndex) return;
    detachFromProvider();
    stageIndex_ = stageIndex;
    attachToProvider();
    Q_EMIT stageIndexChanged();
    update();
}

bool TerminalCustomShaderEffect::isActive() const noexcept
{
    return source_ != nullptr && program_ != nullptr && program_->isValid()
        && providerInterface() != nullptr;
}

QSGNode *
TerminalCustomShaderEffect::updatePaintNode(QSGNode *oldNode,
                                            QQuickItem::UpdatePaintNodeData *)
{
    auto *node = static_cast<TerminalCustomShaderQsgNode *>(oldNode);
    TerminalCustomShaderUniformProvider *const provider = providerInterface();
    if (source_ == nullptr || provider == nullptr || program_ == nullptr
        || !program_->isValid() || !source_->isTextureProvider()) {
        delete node;
        return nullptr;
    }

    QSGTextureProvider *const textureProvider = source_->textureProvider();
    QSGTexture *const texture =
        textureProvider != nullptr ? textureProvider->texture() : nullptr;
    const TerminalCustomShaderUniformSnapshot uniforms =
        provider->terminalCustomShaderUniformSnapshot(stageIndex_);
    if (texture == nullptr || uniforms == nullptr) {
        delete node;
        return nullptr;
    }

    if (node == nullptr) node = new TerminalCustomShaderQsgNode;
    if (!node->update(texture, boundingRect(), program_, std::move(uniforms))) {
        delete node;
        return nullptr;
    }
    return node;
}

void TerminalCustomShaderEffect::geometryChange(const QRectF &newGeometry,
                                                const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) update();
}

TerminalCustomShaderUniformProvider *
TerminalCustomShaderEffect::providerInterface() const noexcept
{
    return qobject_cast<TerminalCustomShaderUniformProvider *>(
        uniformProvider_.data());
}

void TerminalCustomShaderEffect::attachToProvider()
{
    if (TerminalCustomShaderUniformProvider *const provider =
            providerInterface()) {
        provider->terminalCustomShaderEffectAttached(this, stageIndex_);
    }
}

void TerminalCustomShaderEffect::detachFromProvider()
{
    if (TerminalCustomShaderUniformProvider *const provider =
            providerInterface()) {
        provider->terminalCustomShaderEffectDetached(this, stageIndex_);
    }
}

void TerminalCustomShaderEffect::updateActive(bool wasActive)
{
    if (wasActive != isActive()) Q_EMIT activeChanged();
}
