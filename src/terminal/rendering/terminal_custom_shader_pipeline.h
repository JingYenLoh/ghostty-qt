#pragma once

#include "terminal/rendering/terminal_custom_shader_compiler.h"
#include "terminal/rendering/terminal_custom_shader_qsg.h"

#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QSize>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

#include <cstdint>
#include <memory>
#include <optional>

struct TerminalCustomShaderPipelineSnapshot {
    std::uint64_t frameCount = 0;
    std::uint64_t drawCount = 0;
    std::uint64_t targetCreateCount = 0;
    std::uint64_t targetDestroyCount = 0;
    std::uint64_t pipelineCreateCount = 0;
    std::uint64_t bindingCreateCount = 0;
    std::uint64_t sourceBindingUpdateCount = 0;
    std::uint64_t resourceGeneration = 0;
    int liveTargetCount = 0;
    int liveBindingCount = 0;
    int passCount = 0;
    int uniformSlotCount = 0;
    QSize targetPixelSize;
    std::uint64_t ownedTextureBytes = 0;
    std::uint64_t uniformBufferBytes = 0;
    std::uint64_t uniformUploadBytesPerFrame = 0;
    QString diagnostic;

    bool
    operator==(const TerminalCustomShaderPipelineSnapshot &) const = default;
};

[[nodiscard]] int
terminalCustomShaderPipelineTargetCount(qsizetype passCount) noexcept;

[[nodiscard]] int
terminalCustomShaderPipelineBindingCount(qsizetype passCount) noexcept;

struct TerminalCustomShaderUniformSlotPlan {
    QVector<qsizetype> stageSlots;
    QVector<qsizetype> slotStages;

    bool
    operator==(const TerminalCustomShaderUniformSlotPlan &) const = default;
};

[[nodiscard]] std::optional<TerminalCustomShaderUniformSlotPlan>
terminalCustomShaderUniformSlotPlan(
    const QVector<TerminalCustomShaderUniformSnapshot> &uniforms);

[[nodiscard]] QVariantList terminalCustomShaderStagesToVariantList(
    const QVector<TerminalCustomShaderStage> &stages);

struct TerminalCustomShaderPipelineTelemetry;

// One layer-effect item replaces the legacy nested item-per-pass chain. The
// injected source is the pane's one flattened Qt layer. Its render node records
// all intermediate passes into at most two retained textures, then records the
// final pass inline into Qt Quick's active scene render target.
class TerminalCustomShaderPipelineEffect : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(TerminalCustomShaderPipelineEffect)
    Q_PROPERTY(
        QQuickItem *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QVariantList shaderStages READ shaderStages WRITE setShaderStages
                   NOTIFY shaderStagesChanged)
    Q_PROPERTY(QObject *uniformProvider READ uniformProvider WRITE
                   setUniformProvider NOTIFY uniformProviderChanged)
    Q_PROPERTY(bool linearBlending READ linearBlending WRITE setLinearBlending
                   NOTIFY linearBlendingChanged)
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

public:
    explicit TerminalCustomShaderPipelineEffect(QQuickItem *parent = nullptr);
    ~TerminalCustomShaderPipelineEffect() override;

    [[nodiscard]] QQuickItem *source() const noexcept;
    void setSource(QQuickItem *source);

    [[nodiscard]] QVariantList shaderStages() const;
    void setShaderStages(const QVariantList &stages);
    void setStages(const QVector<TerminalCustomShaderStage> &stages);
    [[nodiscard]] const QVector<TerminalCustomShaderStage> &
    stages() const noexcept;

    [[nodiscard]] QObject *uniformProvider() const noexcept;
    void setUniformProvider(QObject *provider);

    [[nodiscard]] bool linearBlending() const noexcept;
    void setLinearBlending(bool enabled);

    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] TerminalCustomShaderPipelineSnapshot renderSnapshot() const;
    [[nodiscard]] QString renderDiagnostic() const;

Q_SIGNALS:
    void sourceChanged();
    void shaderStagesChanged();
    void uniformProviderChanged();
    void linearBlendingChanged();
    void activeChanged();

protected:
    QSGNode *updatePaintNode(
        QSGNode *oldNode,
        QQuickItem::UpdatePaintNodeData *updatePaintNodeData) override;
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override;

private:
    [[nodiscard]] TerminalCustomShaderUniformProvider *
    providerInterface() const noexcept;
    void attachToProvider();
    void detachFromProvider();
    void updateActive(bool wasActive);
    void rebuildPrograms();

    QPointer<QQuickItem> source_;
    QVector<TerminalCustomShaderStage> stages_;
    QVector<std::shared_ptr<const TerminalCustomShaderProgram>> programs_;
    QPointer<QObject> uniformProvider_;
    bool linearBlending_ = false;
    std::shared_ptr<TerminalCustomShaderPipelineTelemetry> telemetry_;
    QString stageDiagnostic_;
    QMetaObject::Connection sourceDestroyedConnection_;
    QMetaObject::Connection providerDestroyedConnection_;
};
