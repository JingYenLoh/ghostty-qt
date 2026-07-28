#pragma once

#include "terminal_backdrop.h"

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QSGGeometryNode>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>

class QQuickWindow;
class QSGGeometry;
class QSGSimpleRectNode;
class QSGSimpleTextureNode;
class QSGTexture;
class TerminalBackdropQsgMaterial;

// Retained, render-thread-only scene-graph node for Ghostty's background-image
// composition. The two input planes must be opaque RGBX8888 images of equal
// size: straightRgbPlane stores straight RGB, while alphaPlane replicates the
// source alpha into RGB. Keeping alpha separate preserves Ghostty's
// interpolate-straight-then-premultiply ordering through Qt's public texture
// upload API.
class TerminalBackdropQsgNode final : public QSGGeometryNode {
public:
    TerminalBackdropQsgNode();
    ~TerminalBackdropQsgNode() override;

    // viewport and destination must use the same pane-local logical coordinate
    // space. A changed, non-zero assetSerial is the authority for replacing
    // the textures; changing either image without changing the serial is
    // intentionally ignored.
    //
    // Returns true when the node is drawable. Invalid state or a texture
    // creation failure clears the geometry and textures, making an already
    // attached node safe while its caller falls back to a solid background.
    [[nodiscard]] bool update(QQuickWindow *window,
                              const QImage &straightRgbPlane,
                              const QImage &alphaPlane, quint64 assetSerial,
                              const QRectF &viewport, const QColor &background,
                              double imageOpacity, bool repeat,
                              const QRectF &destination);

    void clear();

    [[nodiscard]] bool isDrawable() const noexcept;
    [[nodiscard]] quint64 assetSerial() const noexcept;
    [[nodiscard]] quint64 textureGeneration() const noexcept;

private:
    [[nodiscard]] bool replaceTextures(QQuickWindow *window,
                                       const QImage &straightRgbPlane,
                                       const QImage &alphaPlane,
                                       quint64 assetSerial);

    QSGGeometry *geometry_ = nullptr;
    TerminalBackdropQsgMaterial *material_ = nullptr;
    std::unique_ptr<QSGTexture> straightRgbTexture_;
    std::unique_ptr<QSGTexture> alphaTexture_;
    QQuickWindow *textureWindow_ = nullptr;
    quint64 assetSerial_ = 0;
    quint64 textureGeneration_ = 0;
    QRectF viewport_;
};

// Retained full-backdrop owner used by TerminalPane. It chooses the exact
// two-plane material on RHI renderers and contains the source-resolution CPU
// fallback for Qt's software adaptation. All texture and child-node ownership
// stays on the scene-graph thread.
class TerminalBackdropSceneNode final : public QSGNode {
public:
    TerminalBackdropSceneNode();
    ~TerminalBackdropSceneNode() override;

    void update(
        QQuickWindow *window, const QRectF &viewport, const QColor &background,
        const std::shared_ptr<const TerminalBackgroundImageAsset> &asset,
        const TerminalBackgroundImageOptions &options, qreal devicePixelRatio,
        bool useCustomMaterial);

    [[nodiscard]] quint64 assetSerial() const noexcept;
    [[nodiscard]] const QRectF &imageRect() const noexcept;
    [[nodiscard]] const QRectF &sourceRect() const noexcept;
    [[nodiscard]] std::array<QRectF, 4> baseRects() const noexcept;

private:
    struct MaterialFailureKey {
        quint64 assetSerial = 0;
        QQuickWindow *window = nullptr;

        bool operator==(const MaterialFailureKey &) const = default;
    };

    struct CpuTextureKey {
        quint64 assetSerial = 0;
        QQuickWindow *window = nullptr;
        QRgb background = 0;
        quint64 imageOpacityBits = 0;

        bool operator==(const CpuTextureKey &) const = default;
    };

    void clearCpuTexture();
    void setBase(std::size_t index, const QRectF &rect,
                 const QColor &color);
    void clearBases(const QColor &color);
    void setSolidBase(const QRectF &viewport, const QColor &color);

    TerminalBackdropQsgNode *hardwareBackdrop_ = nullptr;
    std::array<QSGSimpleRectNode *, 4> baseBackgrounds_{};
    QSGSimpleTextureNode *cpuImage_ = nullptr;
    std::unique_ptr<QSGTexture> texture_;
    std::optional<CpuTextureKey> textureKey_;
    std::optional<CpuTextureKey> failedTextureKey_;
    std::optional<MaterialFailureKey> failedMaterialKey_;
    bool textureRepeat_ = false;
    QRectF imageRect_;
    QRectF sourceRect_;
};
