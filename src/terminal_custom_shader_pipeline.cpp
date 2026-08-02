#include "terminal_custom_shader_pipeline.h"

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QMatrix4x4>
#include <QMutex>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QRect>
#include <QResource>
#include <QSGRenderNode>
#include <QSGTexture>
#include <QSGTextureProvider>
#include <QVariantMap>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

struct TerminalCustomShaderPipelineTelemetry {
    mutable QMutex mutex;
    TerminalCustomShaderPipelineSnapshot snapshot;
};

static void initializeTerminalCustomShaderPipelineResources()
{
    static const bool initialized = [] {
        Q_INIT_RESOURCE(ghostty_qt_terminal_custom_shader_shaders);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace {

struct Vertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr qsizetype verticesPerQuad = 4;
constexpr qsizetype quadByteSize =
    verticesPerQuad * static_cast<qsizetype>(sizeof(Vertex));
constexpr qsizetype vertexBufferByteSize = quadByteSize * 2;

class DebugMarkerScope final {
public:
    DebugMarkerScope(QRhiCommandBuffer *commandBuffer, const QByteArray &label,
                     bool enabled)
        : commandBuffer_(enabled ? commandBuffer : nullptr)
    {
        if (commandBuffer_ != nullptr) commandBuffer_->debugMarkBegin(label);
    }

    ~DebugMarkerScope()
    {
        if (commandBuffer_ != nullptr) commandBuffer_->debugMarkEnd();
    }

    Q_DISABLE_COPY_MOVE(DebugMarkerScope)

private:
    QRhiCommandBuffer *commandBuffer_;
};

const QShader &terminalCustomShaderVertexShader()
{
    static const QShader shader = [] {
        initializeTerminalCustomShaderPipelineResources();
        QFile file(QStringLiteral(
            ":/ghostty-qt/shaders/terminal_custom_shader.vert.qsb"));
        if (!file.open(QIODevice::ReadOnly)) return QShader{};
        return QShader::fromSerialized(file.readAll());
    }();
    return shader;
}

void setTelemetryDiagnostic(
    const std::shared_ptr<TerminalCustomShaderPipelineTelemetry> &telemetry,
    const QString &diagnostic)
{
    QMutexLocker locker(&telemetry->mutex);
    telemetry->snapshot.diagnostic = diagnostic;
}

void setUniformMatrix(TerminalCustomShaderUniforms *uniforms,
                      const QMatrix4x4 &matrix)
{
    static_assert(sizeof(float) * 16
                  == sizeof(TerminalCustomShaderUniforms::qtMatrix));
    std::memcpy(uniforms->qtMatrix.data(), matrix.constData(),
                sizeof(float) * 16);
}

bool samePrograms(
    const QVector<std::shared_ptr<const TerminalCustomShaderProgram>> &left,
    const QVector<std::shared_ptr<const TerminalCustomShaderProgram>> &right)
{
    if (left.size() != right.size()) return false;
    for (qsizetype index = 0; index < left.size(); ++index) {
        if (left.at(index) != right.at(index)) return false;
    }
    return true;
}

bool buildTerminalCustomShaderUniformSlotPlan(
    const QVector<TerminalCustomShaderUniformSnapshot> &uniforms,
    QVector<qsizetype> *stageSlots, QVector<qsizetype> *slotStages)
{
    Q_ASSERT(stageSlots != nullptr);
    Q_ASSERT(slotStages != nullptr);
    stageSlots->clear();
    slotStages->clear();
    if (uniforms.isEmpty()
        || std::ranges::any_of(
            uniforms, [](const TerminalCustomShaderUniformSnapshot &snapshot) {
                return snapshot == nullptr;
            })) {
        return false;
    }

    stageSlots->resize(uniforms.size());
    slotStages->reserve(uniforms.size());
    const qsizetype finalStage = uniforms.size() - 1;
    for (qsizetype stage = 0; stage < uniforms.size(); ++stage) {
        const bool final = stage == finalStage;
        qsizetype slot = -1;
        for (qsizetype candidate = 0; candidate < slotStages->size();
             ++candidate) {
            const qsizetype candidateStage = slotStages->at(candidate);
            if ((candidateStage == finalStage) == final
                && uniforms.at(candidateStage).get()
                    == uniforms.at(stage).get()) {
                slot = candidate;
                break;
            }
        }
        if (slot < 0) {
            slot = slotStages->size();
            slotStages->append(stage);
        }
        (*stageSlots)[stage] = slot;
    }
    return true;
}

class TerminalCustomShaderPipelineNode final : public QSGRenderNode {
public:
    explicit TerminalCustomShaderPipelineNode(
        QQuickWindow *window,
        std::shared_ptr<TerminalCustomShaderPipelineTelemetry> telemetry)
        : window_(window)
        , telemetry_(std::move(telemetry))
        , debugMarkersEnabled_(terminalCustomShaderDebugMarkersEnabled(window))
    {
        setFlag(QSGNode::UsePreprocess);
    }

    ~TerminalCustomShaderPipelineNode() override { releaseResources(); }

    void setState(
        QSGTexture *source, const QRectF &viewport,
        QVector<std::shared_ptr<const TerminalCustomShaderProgram>> programs,
        QVector<TerminalCustomShaderUniformSnapshot> uniforms,
        bool linearBlending)
    {
        source_ = source;
        viewport_ = viewport;
        if (!samePrograms(programs_, programs)) {
            programs_ = std::move(programs);
            programsChanged_ = true;
        }
        uniforms_ = std::move(uniforms);
        if (linearBlending_ != linearBlending) {
            linearBlending_ = linearBlending;
            resetTargets();
            programsChanged_ = true;
        }
        (void)buildTerminalCustomShaderUniformSlotPlan(
            uniforms_, &stageUniformSlots_, &uniformSlotStages_);

        QMutexLocker locker(&telemetry_->mutex);
        telemetry_->snapshot.passCount = static_cast<int>(programs_.size());
    }

    void prepare() override
    {
        prepared_ = false;
        if (window_ == nullptr || source_ == nullptr || programs_.isEmpty()
            || programs_.size() != uniforms_.size() || viewport_.isEmpty()) {
            setTelemetryDiagnostic(
                telemetry_,
                QStringLiteral("custom-shader: retained pipeline has "
                               "incomplete render state"));
            return;
        }
        if (stageUniformSlots_.size() != programs_.size()
            || uniformSlotStages_.isEmpty()) {
            setTelemetryDiagnostic(
                telemetry_,
                QStringLiteral("custom-shader: retained pipeline has an "
                               "invalid uniform-slot plan"));
            return;
        }

        QRhi *const nextRhi = window_->rhi();
        QRhiCommandBuffer *const cb = commandBuffer();
        QRhiRenderTarget *const finalTarget = renderTarget();
        if (nextRhi == nullptr || cb == nullptr || finalTarget == nullptr
            || finalTarget->renderPassDescriptor() == nullptr) {
            setTelemetryDiagnostic(
                telemetry_,
                QStringLiteral("custom-shader: Qt did not expose an active "
                               "RHI command buffer or render target"));
            return;
        }
        if (rhi_ != nextRhi) {
            releaseResources();
            rhi_ = nextRhi;
            programsChanged_ = true;
        }

        QRhiResourceUpdateBatch *resourceUpdates =
            rhi_->nextResourceUpdateBatch();
        source_->commitTextureOperations(rhi_, resourceUpdates);
        QRhiTexture *const sourceTexture = source_->rhiTexture();
        if (sourceTexture == nullptr || sourceTexture->rhi() != rhi_
            || sourceTexture->pixelSize().isEmpty()
            || source_->isAtlasTexture()) {
            cb->resourceUpdate(resourceUpdates);
            setTelemetryDiagnostic(
                telemetry_,
                QStringLiteral("custom-shader: the flattened terminal layer "
                               "did not provide a usable RHI texture"));
            return;
        }

        const QSize targetSize = sourceTexture->pixelSize();
        const int targetCount =
            terminalCustomShaderPipelineTargetCount(programs_.size());
        if (!ensureTargets(targetSize, targetCount) || !ensureSharedResources()
            || !ensurePassResources(sourceTexture, finalTarget)) {
            cb->resourceUpdate(resourceUpdates);
            return;
        }

        updateVertexBuffer(resourceUpdates);
        if (!updateUniformBuffer()) {
            cb->resourceUpdate(resourceUpdates);
            return;
        }

        const qsizetype finalIndex = programs_.size() - 1;
        for (qsizetype index = 0; index < finalIndex; ++index) {
            const PassResources &resources =
                passResources_.at(static_cast<std::size_t>(index));
            const DebugMarkerScope marker(cb, resources.debugMarkerLabel,
                                          debugMarkersEnabled_);
            const int targetIndex =
                static_cast<int>(index % static_cast<qsizetype>(targetCount));
            QRhiTextureRenderTarget *const target =
                pingTargets_.at(static_cast<std::size_t>(targetIndex)).get();
            cb->beginPass(target, Qt::transparent, {1.0F, 0},
                          index == 0 ? resourceUpdates : nullptr);
            recordDraw(cb, resources.pipeline.get(), resources.bindings, index,
                       index == 0 ? 0 : quadByteSize, targetSize);
            cb->endPass();
        }
        if (finalIndex == 0) cb->resourceUpdate(resourceUpdates);

        prepared_ = true;
        setTelemetryDiagnostic(telemetry_, {});
    }

    void preprocess() override
    {
        // A layer source is a QSGDynamicTexture. Qt renders dynamic textures
        // during scene-graph preprocessing, before QSGRenderNode::prepare()
        // records the retained post-processing passes.
        if (auto *const dynamicTexture =
                qobject_cast<QSGDynamicTexture *>(source_)) {
            (void)dynamicTexture->updateTexture();
        }
    }

    void render(const RenderState *state) override
    {
        if (!prepared_ || commandBuffer() == nullptr
            || renderTarget() == nullptr
            || passResources_.size()
                != static_cast<std::size_t>(programs_.size())) {
            return;
        }

        const QSize targetSize = renderTarget()->pixelSize();
        QRect scissor(QPoint(0, 0), targetSize);
        if (state != nullptr && state->scissorEnabled()) {
            scissor = scissor.intersected(state->scissorRect());
        }
        if (scissor.isEmpty()) return;

        const qsizetype finalIndex = programs_.size() - 1;
        PassResources &resources =
            passResources_[static_cast<std::size_t>(finalIndex)];
        const bool stencil = state != nullptr && state->stencilEnabled();
        QRhiGraphicsPipeline *const pipeline = stencil
            ? resources.stencilPipeline.get()
            : resources.pipeline.get();
        if (pipeline == nullptr) return;

        QRhiCommandBuffer *const cb = commandBuffer();
        const DebugMarkerScope marker(cb, resources.debugMarkerLabel,
                                      debugMarkersEnabled_);
        cb->setGraphicsPipeline(pipeline);
        cb->setViewport(QRhiViewport(0.0F, 0.0F,
                                     static_cast<float>(targetSize.width()),
                                     static_cast<float>(targetSize.height())));
        cb->setScissor(QRhiScissor(scissor.x(), scissor.y(), scissor.width(),
                                   scissor.height()));
        if (stencil) {
            cb->setStencilRef(
                static_cast<quint32>(std::max(0, state->stencilValue())));
        }

        const QRhiCommandBuffer::DynamicOffset dynamicOffset{
            0,
            static_cast<quint32>(stageUniformSlots_.at(finalIndex)
                                 * uniformStride_)};
        cb->setShaderResources(resources.bindings, 1, &dynamicOffset);
        const QRhiCommandBuffer::VertexInput vertexBinding{
            vertexBuffer_.get(), finalIndex == 0 ? 0 : quadByteSize};
        cb->setVertexInput(0, 1, &vertexBinding);
        cb->draw(static_cast<quint32>(verticesPerQuad));

        QMutexLocker locker(&telemetry_->mutex);
        ++telemetry_->snapshot.frameCount;
        telemetry_->snapshot.drawCount +=
            static_cast<std::uint64_t>(programs_.size());
    }

    void releaseResources() override
    {
        resetPassResources();
        sampler_.reset();
        uniformBuffer_.reset();
        vertexBuffer_.reset();
        passInputs_.clear();
        vertexDataDirty_ = true;
        uploadedViewport_ = {};
        uploadedSourceCoordinates_ = {};
        uniformStride_ = 0;
        uniformBufferSize_ = 0;
        {
            QMutexLocker locker(&telemetry_->mutex);
            telemetry_->snapshot.uniformSlotCount = 0;
            telemetry_->snapshot.uniformBufferBytes = 0;
            telemetry_->snapshot.uniformUploadBytesPerFrame = 0;
        }
        resetTargets();
        rhi_ = nullptr;
        prepared_ = false;
        programsChanged_ = true;
    }

    RenderingFlags flags() const override
    {
        return NoExternalRendering | DepthAwareRendering | BoundedRectRendering;
    }

    StateFlags changedStates() const override
    {
        return ViewportState | ScissorState;
    }

    QRectF rect() const override { return viewport_; }

private:
    struct BindingResources {
        QRhiTexture *input = nullptr;
        std::unique_ptr<QRhiShaderResourceBindings> bindings;
    };

    struct PassResources {
        QRhiShaderResourceBindings *bindings = nullptr;
        std::unique_ptr<QRhiGraphicsPipeline> pipeline;
        std::unique_ptr<QRhiGraphicsPipeline> stencilPipeline;
        QByteArray debugMarkerLabel;
    };

    void resetPassResources()
    {
        passResources_.clear();
        const bool hadBindings = !bindingResources_.empty();
        bindingResources_.clear();
        if (hadBindings) {
            QMutexLocker locker(&telemetry_->mutex);
            telemetry_->snapshot.liveBindingCount = 0;
        }
        builtPrograms_.clear();
        builtBindingInputs_.clear();
        builtPassBindingIndices_.clear();
        builtFinalRenderPassFormat_.clear();
        builtFinalSampleCount_ = 0;
        builtNeedsStencil_ = false;
        builtUniformBuffer_ = nullptr;
    }

    void resetTargets()
    {
        // Pipelines and bindings borrow the ping render-pass descriptor and
        // textures, so release them before invalidating either dependency.
        resetPassResources();
        const bool hadTargetPlan = targetPlanInitialized_;
        int destroyed = 0;
        for (const std::unique_ptr<QRhiTexture> &texture : pingTextures_) {
            if (texture != nullptr) ++destroyed;
        }
        pingTargets_.clear();
        pingRenderPass_.reset();
        pingTextures_.clear();
        targetSize_ = {};
        targetCount_ = 0;
        targetPlanInitialized_ = false;

        if (hadTargetPlan || destroyed > 0) {
            QMutexLocker locker(&telemetry_->mutex);
            telemetry_->snapshot.targetDestroyCount +=
                static_cast<std::uint64_t>(destroyed);
            telemetry_->snapshot.liveTargetCount = 0;
            telemetry_->snapshot.targetPixelSize = {};
            telemetry_->snapshot.ownedTextureBytes = 0;
        }
    }

    bool ensureTargets(const QSize &size, int count)
    {
        if (targetPlanInitialized_ && targetSize_ == size
            && targetCount_ == count) {
            return true;
        }
        if (targetPlanInitialized_ && targetCount_ == count
            && pingTextures_.size() == static_cast<std::size_t>(count)
            && pingTargets_.size() == static_cast<std::size_t>(count)) {
            for (int index = 0; index < count; ++index) {
                QRhiTexture *const texture =
                    pingTextures_.at(static_cast<std::size_t>(index)).get();
                QRhiTextureRenderTarget *const target =
                    pingTargets_.at(static_cast<std::size_t>(index)).get();
                texture->setPixelSize(size);
                if (!texture->create() || !target->create()) {
                    setTelemetryDiagnostic(
                        telemetry_,
                        QStringLiteral("custom-shader: unable to resize "
                                       "retained render target %1")
                            .arg(index + 1));
                    resetTargets();
                    return false;
                }
            }
            targetSize_ = size;
            ++targetGeneration_;
            {
                QMutexLocker locker(&telemetry_->mutex);
                telemetry_->snapshot.targetPixelSize = size;
                telemetry_->snapshot.ownedTextureBytes =
                    static_cast<std::uint64_t>(count)
                    * static_cast<std::uint64_t>(size.width())
                    * static_cast<std::uint64_t>(size.height())
                    * (linearBlending_ ? 8U : 4U);
                telemetry_->snapshot.resourceGeneration = targetGeneration_;
            }
            return true;
        }

        resetTargets();
        targetPlanInitialized_ = true;
        targetSize_ = size;
        targetCount_ = count;
        pingTextures_.reserve(static_cast<std::size_t>(count));
        pingTargets_.reserve(static_cast<std::size_t>(count));

        for (int index = 0; index < count; ++index) {
            const QRhiTexture::Format format =
                linearBlending_ ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
            std::unique_ptr<QRhiTexture> texture(
                rhi_->newTexture(format, size, 1, QRhiTexture::RenderTarget));
            if (texture == nullptr) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to allocate retained "
                                   "%1 render target %2")
                        .arg(linearBlending_ ? QStringLiteral("RGBA16F")
                                             : QStringLiteral("RGBA8"))
                        .arg(index + 1));
                resetTargets();
                return false;
            }
            texture->setName(QByteArrayLiteral("ghostty-qt custom shader ping ")
                             + QByteArray::number(index));
            if (!texture->create()) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to allocate retained "
                                   "%1 render target %2")
                        .arg(linearBlending_ ? QStringLiteral("RGBA16F")
                                             : QStringLiteral("RGBA8"))
                        .arg(index + 1));
                resetTargets();
                return false;
            }

            std::unique_ptr<QRhiTextureRenderTarget> target(
                rhi_->newTextureRenderTarget(
                    QRhiTextureRenderTargetDescription(texture.get())));
            if (target == nullptr) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to create retained "
                                   "render target %1")
                        .arg(index + 1));
                resetTargets();
                return false;
            }
            if (pingRenderPass_ == nullptr) {
                pingRenderPass_.reset(
                    target->newCompatibleRenderPassDescriptor());
                if (pingRenderPass_ != nullptr) {
                    pingRenderPass_->setName(QByteArrayLiteral(
                        "ghostty-qt custom shader ping render pass"));
                }
            }
            if (pingRenderPass_ == nullptr) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to create a retained "
                                   "render-pass descriptor"));
                resetTargets();
                return false;
            }
            target->setRenderPassDescriptor(pingRenderPass_.get());
            target->setName(
                QByteArrayLiteral("ghostty-qt custom shader target ")
                + QByteArray::number(index));
            if (!target->create()) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to initialize "
                                   "retained render target %1")
                        .arg(index + 1));
                resetTargets();
                return false;
            }
            pingTextures_.push_back(std::move(texture));
            pingTargets_.push_back(std::move(target));
            {
                QMutexLocker locker(&telemetry_->mutex);
                ++telemetry_->snapshot.targetCreateCount;
            }
        }

        ++targetGeneration_;
        programsChanged_ = true;
        {
            QMutexLocker locker(&telemetry_->mutex);
            telemetry_->snapshot.liveTargetCount = count;
            telemetry_->snapshot.targetPixelSize = size;
            telemetry_->snapshot.ownedTextureBytes =
                static_cast<std::uint64_t>(count)
                * static_cast<std::uint64_t>(size.width())
                * static_cast<std::uint64_t>(size.height())
                * (linearBlending_ ? 8U : 4U);
            telemetry_->snapshot.resourceGeneration = targetGeneration_;
        }
        return true;
    }

    bool ensureSharedResources()
    {
        if (vertexBuffer_ == nullptr) {
            vertexBuffer_.reset(
                rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                static_cast<quint32>(vertexBufferByteSize)));
            if (vertexBuffer_ == nullptr) {
                vertexBuffer_.reset();
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to create retained "
                                   "vertex buffer"));
                return false;
            }
            vertexBuffer_->setName(
                QByteArrayLiteral("ghostty-qt custom shader vertices"));
            if (!vertexBuffer_->create()) {
                vertexBuffer_.reset();
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to create retained "
                                   "vertex buffer"));
                return false;
            }
            vertexDataDirty_ = true;
        }

        const int stride = rhi_->ubufAligned(
            static_cast<int>(TerminalCustomShaderUniformLayout::size));
        const qsizetype requestedSize =
            static_cast<qsizetype>(stride) * uniformSlotStages_.size();
        if (stride <= 0 || requestedSize <= 0
            || requestedSize
                > static_cast<qsizetype>(std::numeric_limits<quint32>::max())) {
            setTelemetryDiagnostic(
                telemetry_,
                QStringLiteral("custom-shader: retained uniform buffer size "
                               "exceeds the RHI limit"));
            return false;
        }
        if (uniformBuffer_ == nullptr || uniformStride_ != stride
            || uniformBufferSize_ != requestedSize) {
            if (uniformBuffer_ == nullptr) {
                uniformBuffer_.reset(rhi_->newBuffer(
                    QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                    static_cast<quint32>(requestedSize)));
            } else {
                // Keep the QRhiBuffer object stable: retained SRBs borrow its
                // address even while its native allocation is recreated.
                uniformBuffer_->setSize(static_cast<quint32>(requestedSize));
            }
            if (uniformBuffer_ == nullptr) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to create retained "
                                   "uniform buffer"));
                return false;
            }
            uniformBuffer_->setName(
                QByteArrayLiteral("ghostty-qt custom shader uniforms"));
            if (!uniformBuffer_->create()) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to create retained "
                                   "uniform buffer"));
                return false;
            }
            uniformStride_ = stride;
            uniformBufferSize_ = requestedSize;
            programsChanged_ = true;
            QMutexLocker locker(&telemetry_->mutex);
            telemetry_->snapshot.uniformSlotCount =
                static_cast<int>(uniformSlotStages_.size());
            telemetry_->snapshot.uniformBufferBytes =
                static_cast<std::uint64_t>(requestedSize);
            telemetry_->snapshot.uniformUploadBytesPerFrame =
                static_cast<std::uint64_t>(uniformSlotStages_.size())
                * TerminalCustomShaderUniformLayout::size;
        }

        if (sampler_ == nullptr) {
            sampler_.reset(rhi_->newSampler(
                QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge,
                QRhiSampler::ClampToEdge));
            if (sampler_ == nullptr) {
                sampler_.reset();
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to create retained "
                                   "texture sampler"));
                return false;
            }
            sampler_->setName(
                QByteArrayLiteral("ghostty-qt custom shader sampler"));
            if (!sampler_->create()) {
                sampler_.reset();
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to create retained "
                                   "texture sampler"));
                return false;
            }
            programsChanged_ = true;
        }
        return true;
    }

    const QVector<QRhiTexture *> &passInputs(QRhiTexture *sourceTexture)
    {
        passInputs_.resize(programs_.size());
        for (qsizetype index = 0; index < programs_.size(); ++index) {
            if (index == 0) {
                passInputs_[index] = sourceTexture;
            } else {
                const qsizetype targetIndex = (index - 1)
                    % static_cast<qsizetype>(std::max(1, targetCount_));
                passInputs_[index] =
                    pingTextures_.at(static_cast<std::size_t>(targetIndex))
                        .get();
            }
        }
        return passInputs_;
    }

    void planInputBindings(const QVector<QRhiTexture *> &inputs)
    {
        bindingInputs_.clear();
        bindingInputs_.reserve(inputs.size());
        passBindingIndices_.resize(inputs.size());
        for (qsizetype passIndex = 0; passIndex < inputs.size(); ++passIndex) {
            QRhiTexture *const input = inputs.at(passIndex);
            qsizetype bindingIndex = bindingInputs_.indexOf(input);
            if (bindingIndex < 0) {
                bindingIndex = bindingInputs_.size();
                bindingInputs_.append(inputs.at(passIndex));
            }
            passBindingIndices_[passIndex] = bindingIndex;
        }
    }

    void setBindingResources(QRhiShaderResourceBindings *bindings,
                             QRhiTexture *input)
    {
        bindings->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                0,
                QRhiShaderResourceBinding::VertexStage
                    | QRhiShaderResourceBinding::FragmentStage,
                uniformBuffer_.get(),
                static_cast<quint32>(TerminalCustomShaderUniformLayout::size)),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage, input,
                sampler_.get()),
        });
    }

    bool createBindingResources()
    {
        bindingResources_.clear();
        bindingResources_.reserve(
            static_cast<std::size_t>(bindingInputs_.size()));
        for (qsizetype index = 0; index < bindingInputs_.size(); ++index) {
            BindingResources resources;
            resources.input = bindingInputs_.at(index);
            resources.bindings.reset(rhi_->newShaderResourceBindings());
            if (resources.bindings == nullptr) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to allocate retained "
                                   "resource bindings for input %1")
                        .arg(index + 1));
                return false;
            }
            setBindingResources(resources.bindings.get(), resources.input);
            resources.bindings->setName(
                QByteArrayLiteral("ghostty-qt custom shader bindings ")
                + (index == 0 ? QByteArrayLiteral("source")
                              : QByteArrayLiteral("ping ")
                           + QByteArray::number(index - 1)));
            if (!resources.bindings->create()) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to create retained "
                                   "resource bindings for input %1")
                        .arg(index + 1));
                return false;
            }
            {
                QMutexLocker locker(&telemetry_->mutex);
                ++telemetry_->snapshot.bindingCreateCount;
            }
            if (!bindingResources_.empty()
                && !bindingResources_.front().bindings->isLayoutCompatible(
                    resources.bindings.get())) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: retained resource bindings "
                                   "have incompatible layouts"));
                return false;
            }
            bindingResources_.push_back(std::move(resources));
        }

        QMutexLocker locker(&telemetry_->mutex);
        telemetry_->snapshot.liveBindingCount =
            static_cast<int>(bindingResources_.size());
        return true;
    }

    std::unique_ptr<QRhiGraphicsPipeline>
    createPipeline(qsizetype stageIndex, QRhiShaderResourceBindings *bindings,
                   QRhiRenderPassDescriptor *renderPassDescriptor,
                   int sampleCount, bool final, bool stencil)
    {
        const QShader &vertexShader = terminalCustomShaderVertexShader();
        if (!vertexShader.isValid()) return {};

        std::unique_ptr<QRhiGraphicsPipeline> pipeline(
            rhi_->newGraphicsPipeline());
        if (pipeline == nullptr) return {};
        pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        pipeline->setShaderStages({
            {QRhiShaderStage::Vertex, vertexShader},
            {QRhiShaderStage::Fragment, programs_.at(stageIndex)->shader()},
        });
        QRhiVertexInputLayout inputLayout;
        inputLayout.setBindings({{static_cast<quint32>(sizeof(Vertex))}});
        inputLayout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0},
            {0, 1, QRhiVertexInputAttribute::Float2,
             2U * static_cast<quint32>(sizeof(float))},
        });
        pipeline->setVertexInputLayout(inputLayout);
        pipeline->setShaderResourceBindings(bindings);
        pipeline->setRenderPassDescriptor(renderPassDescriptor);
        pipeline->setSampleCount(sampleCount);

        if (final) {
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::One;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            pipeline->setTargetBlends({blend});
            QRhiGraphicsPipeline::Flags flags =
                QRhiGraphicsPipeline::UsesScissor;
            if (stencil) {
                flags |= QRhiGraphicsPipeline::UsesStencilRef;
                QRhiGraphicsPipeline::StencilOpState stencilState;
                stencilState.compareOp = QRhiGraphicsPipeline::Equal;
                pipeline->setStencilTest(true);
                pipeline->setStencilFront(stencilState);
                pipeline->setStencilBack(stencilState);
            }
            pipeline->setFlags(flags);
        }

        pipeline->setName(
            QByteArrayLiteral("ghostty-qt custom shader pass ")
            + QByteArray::number(stageIndex)
            + (final ? QByteArrayLiteral(" final")
                     : QByteArrayLiteral(" intermediate"))
            + (stencil ? QByteArrayLiteral(" stencil") : QByteArray{}));
        if (!pipeline->create()) return {};
        {
            QMutexLocker locker(&telemetry_->mutex);
            ++telemetry_->snapshot.pipelineCreateCount;
        }
        return pipeline;
    }

    bool ensurePassResources(QRhiTexture *sourceTexture,
                             QRhiRenderTarget *finalTarget)
    {
        // A rectangular clip node is only a hint. Qt may still implement it
        // with stencil after a non-axis-aligned transform, so retain the final
        // stencil variant whenever any ancestor clip exists.
        const bool needsStencil = clipList() != nullptr;
        const QVector<quint32> finalFormat =
            finalTarget->renderPassDescriptor()->serializedFormat();
        const int finalSampleCount = finalTarget->sampleCount();
        const QVector<QRhiTexture *> &inputs = passInputs(sourceTexture);
        planInputBindings(inputs);
        const bool rebuild = programsChanged_
            || !samePrograms(programs_, builtPrograms_)
            || passResources_.size()
                != static_cast<std::size_t>(programs_.size())
            || bindingResources_.size()
                != static_cast<std::size_t>(bindingInputs_.size())
            || builtBindingInputs_.size() != bindingInputs_.size()
            || builtPassBindingIndices_ != passBindingIndices_
            || builtUniformBuffer_ != uniformBuffer_.get()
            || builtFinalRenderPassFormat_ != finalFormat
            || builtFinalSampleCount_ != finalSampleCount
            || builtNeedsStencil_ != needsStencil;

        if (!rebuild) {
            std::uint64_t updateCount = 0;
            for (qsizetype index = 0; index < bindingInputs_.size(); ++index) {
                if (builtBindingInputs_.at(index) == bindingInputs_.at(index)) {
                    continue;
                }
                BindingResources &resources =
                    bindingResources_[static_cast<std::size_t>(index)];
                resources.input = bindingInputs_.at(index);
                setBindingResources(resources.bindings.get(), resources.input);
                resources.bindings->updateResources(
                    QRhiShaderResourceBindings::BindingsAreSorted);
                builtBindingInputs_[index] = bindingInputs_.at(index);
                ++updateCount;
            }
            if (updateCount > 0) {
                QMutexLocker locker(&telemetry_->mutex);
                telemetry_->snapshot.sourceBindingUpdateCount += updateCount;
            }
            return true;
        }

        resetPassResources();
        if (!createBindingResources()) {
            resetPassResources();
            return false;
        }
        passResources_.resize(static_cast<std::size_t>(programs_.size()));
        for (qsizetype index = 0; index < programs_.size(); ++index) {
            PassResources &resources =
                passResources_[static_cast<std::size_t>(index)];
            const bool final = index == programs_.size() - 1;
            resources.debugMarkerLabel = terminalCustomShaderDebugMarkerLabel(
                TerminalCustomShaderRenderPath::Retained, index,
                programs_.size(),
                final ? TerminalCustomShaderPassRole::Final
                      : TerminalCustomShaderPassRole::Intermediate);
            const qsizetype bindingIndex = passBindingIndices_.at(index);
            resources.bindings =
                bindingResources_.at(static_cast<std::size_t>(bindingIndex))
                    .bindings.get();

            QRhiRenderPassDescriptor *const descriptor = final
                ? finalTarget->renderPassDescriptor()
                : pingRenderPass_.get();
            const int sampleCount = final ? finalSampleCount : 1;
            resources.pipeline =
                createPipeline(index, resources.bindings, descriptor,
                               sampleCount, final, false);
            if (resources.pipeline == nullptr) {
                setTelemetryDiagnostic(
                    telemetry_,
                    QStringLiteral("custom-shader: unable to create retained "
                                   "graphics pipeline for pass %1")
                        .arg(index + 1));
                resetPassResources();
                return false;
            }
            if (final && needsStencil) {
                resources.stencilPipeline =
                    createPipeline(index, resources.bindings, descriptor,
                                   sampleCount, true, true);
                if (resources.stencilPipeline == nullptr) {
                    setTelemetryDiagnostic(
                        telemetry_,
                        QStringLiteral(
                            "custom-shader: unable to create retained stencil "
                            "pipeline for the final pass"));
                    resetPassResources();
                    return false;
                }
            }
        }

        builtPrograms_ = programs_;
        builtBindingInputs_ = bindingInputs_;
        builtPassBindingIndices_ = passBindingIndices_;
        builtUniformBuffer_ = uniformBuffer_.get();
        builtFinalRenderPassFormat_ = finalFormat;
        builtFinalSampleCount_ = finalSampleCount;
        builtNeedsStencil_ = needsStencil;
        programsChanged_ = false;
        {
            QMutexLocker locker(&telemetry_->mutex);
            ++telemetry_->snapshot.sourceBindingUpdateCount;
        }
        return true;
    }

    void updateVertexBuffer(QRhiResourceUpdateBatch *updates)
    {
        const QRectF sourceCoordinates = source_->normalizedTextureSubRect();
        if (!vertexDataDirty_ && uploadedViewport_ == viewport_
            && uploadedSourceCoordinates_ == sourceCoordinates) {
            return;
        }
        const QRectF pingCoordinates(0.0, 1.0, 1.0, -1.0);
        const auto quad = [this](const QRectF &coordinates) {
            const float left = static_cast<float>(viewport_.left());
            const float right = static_cast<float>(viewport_.right());
            const float top = static_cast<float>(viewport_.top());
            const float bottom = static_cast<float>(viewport_.bottom());
            const float u0 = static_cast<float>(coordinates.left());
            const float u1 = static_cast<float>(coordinates.right());
            const float v0 = static_cast<float>(coordinates.top());
            const float v1 = static_cast<float>(coordinates.bottom());
            return std::array<Vertex, verticesPerQuad>{{
                {left, top, u0, v0},
                {left, bottom, u0, v1},
                {right, top, u1, v0},
                {right, bottom, u1, v1},
            }};
        };
        const std::array<Vertex, verticesPerQuad> sourceQuad =
            quad(sourceCoordinates);
        const std::array<Vertex, verticesPerQuad> pingQuad =
            quad(pingCoordinates);
        std::array<Vertex, verticesPerQuad * 2> vertices;
        std::ranges::copy(sourceQuad, vertices.begin());
        std::ranges::copy(pingQuad, vertices.begin() + verticesPerQuad);
        updates->updateDynamicBuffer(vertexBuffer_.get(), 0,
                                     static_cast<quint32>(sizeof(vertices)),
                                     vertices.data());
        uploadedViewport_ = viewport_;
        uploadedSourceCoordinates_ = sourceCoordinates;
        vertexDataDirty_ = false;
    }

    bool updateUniformBuffer()
    {
        Q_ASSERT(uniformBuffer_ != nullptr);
        Q_ASSERT(uniformBuffer_->size()
                 == static_cast<quint32>(uniformBufferSize_));
        Q_ASSERT(stageUniformSlots_.size() == programs_.size());
        Q_ASSERT(!uniformSlotStages_.isEmpty());

        // Every slot read this frame is rewritten below. Direct current-frame
        // mapping avoids copying this medium-sized UBO into a resource-update
        // batch; do not mix the two update models for uniformBuffer_.
        char *const uniformBytes =
            uniformBuffer_->beginFullDynamicBufferUpdateForCurrentFrame();
        if (uniformBytes == nullptr) {
            setTelemetryDiagnostic(
                telemetry_,
                QStringLiteral("custom-shader: unable to map the retained "
                               "uniform buffer for the current frame"));
            return false;
        }

        QMatrix4x4 offscreenMatrix;
        // Keep logical top at texture V=1 on every backend. Applying QRhi's
        // clip-space correction here would invert Vulkan relative to OpenGL.
        offscreenMatrix.ortho(static_cast<float>(viewport_.left()),
                              static_cast<float>(viewport_.right()),
                              static_cast<float>(viewport_.bottom()),
                              static_cast<float>(viewport_.top()), -1.0F, 1.0F);
        const QMatrix4x4 finalMatrix =
            projectionMatrix() != nullptr && matrix() != nullptr
            ? *projectionMatrix() * *matrix()
            : QMatrix4x4{};

        for (qsizetype slotIndex = 0; slotIndex < uniformSlotStages_.size();
             ++slotIndex) {
            const qsizetype stageIndex = uniformSlotStages_.at(slotIndex);
            TerminalCustomShaderUniforms uniforms =
                uniforms_.at(stageIndex) != nullptr
                ? *uniforms_.at(stageIndex)
                : TerminalCustomShaderUniforms{};
            const bool final = stageIndex == programs_.size() - 1;
            setUniformMatrix(&uniforms, final ? finalMatrix : offscreenMatrix);
            uniforms.qtOpacity =
                final ? static_cast<float>(inheritedOpacity()) : 1.0F;
            std::memcpy(uniformBytes + slotIndex * uniformStride_, &uniforms,
                        TerminalCustomShaderUniformLayout::size);
        }
        uniformBuffer_->endFullDynamicBufferUpdateForCurrentFrame();
        return true;
    }

    void recordDraw(QRhiCommandBuffer *cb, QRhiGraphicsPipeline *pipeline,
                    QRhiShaderResourceBindings *bindings, qsizetype stageIndex,
                    qsizetype vertexOffset, const QSize &targetSize)
    {
        cb->setGraphicsPipeline(pipeline);
        cb->setViewport(QRhiViewport(0.0F, 0.0F,
                                     static_cast<float>(targetSize.width()),
                                     static_cast<float>(targetSize.height())));
        const QRhiCommandBuffer::DynamicOffset dynamicOffset{
            0,
            static_cast<quint32>(stageUniformSlots_.at(stageIndex)
                                 * uniformStride_)};
        cb->setShaderResources(bindings, 1, &dynamicOffset);
        const QRhiCommandBuffer::VertexInput vertexBinding{
            vertexBuffer_.get(), static_cast<quint32>(vertexOffset)};
        cb->setVertexInput(0, 1, &vertexBinding);
        cb->draw(static_cast<quint32>(verticesPerQuad));
    }

    QPointer<QQuickWindow> window_;
    std::shared_ptr<TerminalCustomShaderPipelineTelemetry> telemetry_;
    const bool debugMarkersEnabled_;
    QSGTexture *source_ = nullptr;
    QRectF viewport_;
    QVector<std::shared_ptr<const TerminalCustomShaderProgram>> programs_;
    QVector<TerminalCustomShaderUniformSnapshot> uniforms_;

    QRhi *rhi_ = nullptr;
    QSize targetSize_;
    int targetCount_ = 0;
    bool targetPlanInitialized_ = false;
    bool linearBlending_ = false;
    std::uint64_t targetGeneration_ = 0;
    std::vector<std::unique_ptr<QRhiTexture>> pingTextures_;
    std::vector<std::unique_ptr<QRhiTextureRenderTarget>> pingTargets_;
    std::unique_ptr<QRhiRenderPassDescriptor> pingRenderPass_;
    std::unique_ptr<QRhiBuffer> vertexBuffer_;
    std::unique_ptr<QRhiBuffer> uniformBuffer_;
    std::unique_ptr<QRhiSampler> sampler_;
    QVector<QRhiTexture *> passInputs_;
    QVector<QRhiTexture *> bindingInputs_;
    QVector<qsizetype> passBindingIndices_;
    QRectF uploadedViewport_;
    QRectF uploadedSourceCoordinates_;
    bool vertexDataDirty_ = true;
    qsizetype uniformStride_ = 0;
    qsizetype uniformBufferSize_ = 0;
    QVector<qsizetype> stageUniformSlots_;
    QVector<qsizetype> uniformSlotStages_;

    std::vector<BindingResources> bindingResources_;
    std::vector<PassResources> passResources_;
    QVector<std::shared_ptr<const TerminalCustomShaderProgram>> builtPrograms_;
    QVector<QRhiTexture *> builtBindingInputs_;
    QVector<qsizetype> builtPassBindingIndices_;
    QVector<quint32> builtFinalRenderPassFormat_;
    int builtFinalSampleCount_ = 0;
    bool builtNeedsStencil_ = false;
    QRhiBuffer *builtUniformBuffer_ = nullptr;
    bool programsChanged_ = true;
    bool prepared_ = false;
};

} // namespace

int terminalCustomShaderPipelineTargetCount(qsizetype passCount) noexcept
{
    if (passCount <= 1) return 0;
    return passCount == 2 ? 1 : 2;
}

int terminalCustomShaderPipelineBindingCount(qsizetype passCount) noexcept
{
    return passCount <= 0
        ? 0
        : 1 + terminalCustomShaderPipelineTargetCount(passCount);
}

std::optional<TerminalCustomShaderUniformSlotPlan>
terminalCustomShaderUniformSlotPlan(
    const QVector<TerminalCustomShaderUniformSnapshot> &uniforms)
{
    TerminalCustomShaderUniformSlotPlan plan;
    if (!buildTerminalCustomShaderUniformSlotPlan(uniforms, &plan.stageSlots,
                                                  &plan.slotStages)) {
        return std::nullopt;
    }
    return plan;
}

QVariantList terminalCustomShaderStagesToVariantList(
    const QVector<TerminalCustomShaderStage> &stages)
{
    QVariantList result;
    result.reserve(stages.size());
    for (const TerminalCustomShaderStage &stage : stages) {
        result.append(QVariantMap{
            {QStringLiteral("sourcePath"), stage.sourcePath},
            {QStringLiteral("qsbPath"), stage.qsbPath},
            {QStringLiteral("cacheKey"), stage.cacheKey},
            {QStringLiteral("serializedShader"), stage.serializedShader},
        });
    }
    return result;
}

TerminalCustomShaderPipelineEffect::TerminalCustomShaderPipelineEffect(
    QQuickItem *parent)
    : QQuickItem(parent)
    , telemetry_(std::make_shared<TerminalCustomShaderPipelineTelemetry>())
{
    setFlag(QQuickItem::ItemHasContents);
}

TerminalCustomShaderPipelineEffect::~TerminalCustomShaderPipelineEffect()
{
    detachFromProvider();
}

QQuickItem *TerminalCustomShaderPipelineEffect::source() const noexcept
{
    return source_;
}

void TerminalCustomShaderPipelineEffect::setSource(QQuickItem *source)
{
    if (source_ == source) return;
    const bool wasActive = isActive();
    QObject::disconnect(sourceDestroyedConnection_);
    source_ = source;
    if (source_ != nullptr) {
        sourceDestroyedConnection_ =
            connect(source_, &QObject::destroyed, this, [this] {
                const bool activeBeforeDestruction =
                    !programs_.isEmpty() && providerInterface() != nullptr;
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

QVariantList TerminalCustomShaderPipelineEffect::shaderStages() const
{
    return terminalCustomShaderStagesToVariantList(stages_);
}

void TerminalCustomShaderPipelineEffect::setShaderStages(
    const QVariantList &stages)
{
    QVector<TerminalCustomShaderStage> parsed;
    parsed.reserve(stages.size());
    QString diagnostic;
    for (qsizetype index = 0; index < stages.size(); ++index) {
        if (!stages.at(index).canConvert<QVariantMap>()) {
            diagnostic =
                QStringLiteral("custom-shader: retained stage %1 is not a map")
                    .arg(index + 1);
            break;
        }
        const QVariantMap map = stages.at(index).toMap();
        TerminalCustomShaderStage stage{
            .sourcePath = map.value(QStringLiteral("sourcePath")).toString(),
            .qsbPath = map.value(QStringLiteral("qsbPath")).toString(),
            .cacheKey = map.value(QStringLiteral("cacheKey")).toByteArray(),
            .serializedShader =
                map.value(QStringLiteral("serializedShader")).toByteArray(),
        };
        if (stage.qsbPath.isEmpty() || stage.serializedShader.isEmpty()) {
            diagnostic =
                QStringLiteral("custom-shader: retained stage %1 is incomplete")
                    .arg(index + 1);
            break;
        }
        parsed.append(std::move(stage));
    }
    if (!diagnostic.isEmpty()) {
        const bool wasActive = isActive();
        const bool changed =
            !stages_.isEmpty() || stageDiagnostic_ != diagnostic;
        stages_.clear();
        programs_.clear();
        stageDiagnostic_ = std::move(diagnostic);
        {
            QMutexLocker locker(&telemetry_->mutex);
            telemetry_->snapshot.passCount = 0;
            telemetry_->snapshot.diagnostic = stageDiagnostic_;
        }
        if (changed) Q_EMIT shaderStagesChanged();
        updateActive(wasActive);
        update();
        return;
    }
    setStages(parsed);
}

void TerminalCustomShaderPipelineEffect::setStages(
    const QVector<TerminalCustomShaderStage> &stages)
{
    const bool diagnosticChanged = !stageDiagnostic_.isEmpty();
    if (stages_ == stages && !diagnosticChanged) return;
    const bool wasActive = isActive();
    stageDiagnostic_.clear();
    stages_ = stages;
    rebuildPrograms();
    {
        QMutexLocker locker(&telemetry_->mutex);
        telemetry_->snapshot.passCount = static_cast<int>(stages_.size());
        telemetry_->snapshot.diagnostic = stageDiagnostic_;
    }
    Q_EMIT shaderStagesChanged();
    updateActive(wasActive);
    update();
}

const QVector<TerminalCustomShaderStage> &
TerminalCustomShaderPipelineEffect::stages() const noexcept
{
    return stages_;
}

QObject *TerminalCustomShaderPipelineEffect::uniformProvider() const noexcept
{
    return uniformProvider_;
}

bool TerminalCustomShaderPipelineEffect::linearBlending() const noexcept
{
    return linearBlending_;
}

void TerminalCustomShaderPipelineEffect::setLinearBlending(bool enabled)
{
    if (linearBlending_ == enabled) return;
    linearBlending_ = enabled;
    Q_EMIT linearBlendingChanged();
    update();
}

void TerminalCustomShaderPipelineEffect::setUniformProvider(QObject *provider)
{
    if (uniformProvider_ == provider) return;
    const bool wasActive = isActive();
    detachFromProvider();
    QObject::disconnect(providerDestroyedConnection_);
    uniformProvider_ = provider;
    if (uniformProvider_ != nullptr) {
        providerDestroyedConnection_ =
            connect(uniformProvider_, &QObject::destroyed, this, [this] {
                const bool activeBeforeDestruction =
                    source_ != nullptr && !programs_.isEmpty();
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

bool TerminalCustomShaderPipelineEffect::isActive() const noexcept
{
    return source_ != nullptr && !programs_.isEmpty()
        && programs_.size() == stages_.size() && providerInterface() != nullptr
        && stageDiagnostic_.isEmpty()
        && std::ranges::all_of(
               programs_,
               [](const std::shared_ptr<const TerminalCustomShaderProgram>
                      &program) {
                   return program != nullptr && program->isValid();
               });
}

TerminalCustomShaderPipelineSnapshot
TerminalCustomShaderPipelineEffect::renderSnapshot() const
{
    QMutexLocker locker(&telemetry_->mutex);
    return telemetry_->snapshot;
}

QString TerminalCustomShaderPipelineEffect::renderDiagnostic() const
{
    return renderSnapshot().diagnostic;
}

QSGNode *TerminalCustomShaderPipelineEffect::updatePaintNode(
    QSGNode *oldNode, QQuickItem::UpdatePaintNodeData *)
{
    auto *node = static_cast<TerminalCustomShaderPipelineNode *>(oldNode);
    TerminalCustomShaderUniformProvider *const provider = providerInterface();
    if (!isActive() || provider == nullptr || !source_->isTextureProvider()
        || window() == nullptr || boundingRect().isEmpty()) {
        delete node;
        return nullptr;
    }
    QSGTextureProvider *const textureProvider = source_->textureProvider();
    QSGTexture *const texture =
        textureProvider != nullptr ? textureProvider->texture() : nullptr;
    if (texture == nullptr) {
        delete node;
        return nullptr;
    }

    QVector<TerminalCustomShaderUniformSnapshot> uniforms;
    uniforms.reserve(programs_.size());
    for (qsizetype index = 0; index < programs_.size(); ++index) {
        TerminalCustomShaderUniformSnapshot snapshot =
            provider->terminalCustomShaderUniformSnapshot(
                static_cast<int>(index));
        if (snapshot == nullptr) {
            delete node;
            return nullptr;
        }
        uniforms.append(std::move(snapshot));
    }

    if (node == nullptr) {
        node = new TerminalCustomShaderPipelineNode(window(), telemetry_);
    }
    node->setState(texture, boundingRect(), programs_, std::move(uniforms),
                   linearBlending_);
    return node;
}

void TerminalCustomShaderPipelineEffect::geometryChange(
    const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) update();
}

TerminalCustomShaderUniformProvider *
TerminalCustomShaderPipelineEffect::providerInterface() const noexcept
{
    return qobject_cast<TerminalCustomShaderUniformProvider *>(
        uniformProvider_.data());
}

void TerminalCustomShaderPipelineEffect::attachToProvider()
{
    if (TerminalCustomShaderUniformProvider *const provider =
            providerInterface()) {
        provider->terminalCustomShaderPipelineAttached(this);
    }
}

void TerminalCustomShaderPipelineEffect::detachFromProvider()
{
    if (TerminalCustomShaderUniformProvider *const provider =
            providerInterface()) {
        provider->terminalCustomShaderPipelineDetached(this);
    }
}

void TerminalCustomShaderPipelineEffect::updateActive(bool wasActive)
{
    if (wasActive != isActive()) Q_EMIT activeChanged();
}

void TerminalCustomShaderPipelineEffect::rebuildPrograms()
{
    programs_.clear();
    programs_.reserve(stages_.size());
    for (qsizetype index = 0; index < stages_.size(); ++index) {
        const TerminalCustomShaderStage &stage = stages_.at(index);
        auto program = std::make_shared<const TerminalCustomShaderProgram>(
            stage.qsbPath, stage.cacheKey, stage.serializedShader);
        if (!program->isValid()) {
            stageDiagnostic_ =
                QStringLiteral("custom-shader: retained stage %1 contains an "
                               "invalid serialized fragment shader")
                    .arg(index + 1);
            programs_.clear();
            return;
        }
        programs_.append(std::move(program));
    }
}
