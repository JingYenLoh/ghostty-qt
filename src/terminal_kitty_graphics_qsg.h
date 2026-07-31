#pragma once

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
inline constexpr qsizetype uniformBufferSize =
    inheritedOpacityOffset + sizeof(float);
} // namespace TerminalKittyGraphicsShaderLayout

// Render-thread-only retained Kitty scene. Placement geometry may be rebuilt
// after scrolling or layout changes while textures remain cached by
// libghostty's globally unique image generation.
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
           bool useCustomMaterial);
    void clear();

    [[nodiscard]] const QVector<TerminalKittyGraphicsRenderPlacement> &
    renderedPlacements() const noexcept;
    [[nodiscard]] quint64 textureUploadCount() const noexcept;
    [[nodiscard]] qsizetype textureCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
