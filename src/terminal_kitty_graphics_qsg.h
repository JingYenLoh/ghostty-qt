#pragma once

#include "terminal_kitty_graphics.h"

#include <QRectF>
#include <QSizeF>
#include <QVector>

#include <memory>

class QQuickWindow;
class QSGNode;

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
