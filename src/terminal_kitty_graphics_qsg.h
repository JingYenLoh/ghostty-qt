#pragma once

#include "terminal_alpha_blending.h"
#include "terminal_kitty_graphics.h"

#include <QRectF>
#include <QSizeF>
#include <QVector>

#include <memory>

class QQuickWindow;
class QSGNode;

// QSGMaterialShader receives a byte array sized from the reflected uniform
// block in the embedded QSB. Keep the CPU writes and shader ABI expressed by
// one layout so a padding assumption cannot overrun Qt's buffer.
namespace TerminalKittyGraphicsShaderLayout {
inline constexpr qsizetype matrixOffset = 0;
inline constexpr qsizetype matrixSize = sizeof(float) * 16;
inline constexpr qsizetype inheritedOpacityOffset = matrixOffset + matrixSize;
inline constexpr qsizetype linearBlendingOffset =
    inheritedOpacityOffset + sizeof(float);
inline constexpr qsizetype uniformBufferSize =
    linearBlendingOffset + sizeof(float);
} // namespace TerminalKittyGraphicsShaderLayout

// Render-thread-only retained Kitty scene. Duplicate-safe placement identities
// retain their nodes across scrolling, layout changes, and same-ID image
// replacement. Textures remain cached by libghostty's globally unique image
// generation.
class TerminalKittyGraphicsScene final {
public:
    TerminalKittyGraphicsScene();
    ~TerminalKittyGraphicsScene();

    TerminalKittyGraphicsScene(const TerminalKittyGraphicsScene &) = delete;
    TerminalKittyGraphicsScene &
    operator=(const TerminalKittyGraphicsScene &) = delete;

    void
    update(QQuickWindow *window, QSGNode *belowBackground, QSGNode *belowText,
           QSGNode *aboveText,
           const std::shared_ptr<const TerminalKittyGraphicsSnapshot> &snapshot,
           const QSizeF &cellSize, const QRectF &gridViewport,
           bool useCustomMaterial,
           TerminalAlphaBlending alphaBlending = TerminalAlphaBlending::Native);
    void clear();

    [[nodiscard]] const QVector<TerminalKittyGraphicsRenderPlacement> &
    renderedPlacements() const noexcept;
    [[nodiscard]] quint64 textureUploadCount() const noexcept;
    // The following five counts are cumulative scene-lifetime churn telemetry;
    // clear() intentionally leaves them intact.
    [[nodiscard]] quint64 nodeCreationCount() const noexcept;
    [[nodiscard]] quint64 nodeDeletionCount() const noexcept;
    [[nodiscard]] quint64 geometryWriteCount() const noexcept;
    [[nodiscard]] quint64 materialAssignmentCount() const noexcept;
    [[nodiscard]] quint64 textureSetEvictionCount() const noexcept;
    [[nodiscard]] qsizetype textureCount() const noexcept;
    // Logical RGBA8 payload resident in live texture sets. Driver allocation
    // overhead and alignment are intentionally not estimated.
    [[nodiscard]] quint64 textureBytes() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
