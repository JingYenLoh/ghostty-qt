#pragma once

#include <QImage>

#include <memory>

class QQuickWindow;
class QSGTexture;
class QRhi;
class QRhiResourceUpdateBatch;
class QRhiTexture;

// Render-thread-only owner for a straight-alpha RGBA8 RHI texture. Qt's
// ordinary QImage upload can premultiply alpha-bearing pixels, so callers use
// this wrapper when filtering must happen before shader premultiplication.
class TerminalStraightRgbaTexture final {
public:
    [[nodiscard]] static std::unique_ptr<TerminalStraightRgbaTexture>
    create(QQuickWindow *window, QImage straightRgba);

    ~TerminalStraightRgbaTexture();
    TerminalStraightRgbaTexture(const TerminalStraightRgbaTexture &) = delete;
    TerminalStraightRgbaTexture &
    operator=(const TerminalStraightRgbaTexture &) = delete;

    [[nodiscard]] QSGTexture *sampledTexture() const noexcept;
    [[nodiscard]] bool
    commitTextureOperations(QRhi *rhi,
                            QRhiResourceUpdateBatch *resourceUpdates);
    [[nodiscard]] quint64 logicalBytes() const noexcept;
    [[nodiscard]] quint64 uploadCount() const noexcept;

private:
    TerminalStraightRgbaTexture(QRhiTexture *raw,
                                std::unique_ptr<QSGTexture> wrapper,
                                QImage straightRgba);

    // createTextureFromRhiTexture transfers QRhiTexture ownership to wrapper_.
    QRhiTexture *raw_ = nullptr;
    std::unique_ptr<QSGTexture> wrapper_;
    QImage straightRgba_;
    bool uploadPending_ = true;
    quint64 uploadCount_ = 0;
};
