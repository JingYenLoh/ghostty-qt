#include "terminal/rendering/terminal_straight_rgba_texture_p.h"

#include <QByteArray>
#include <QQuickWindow>
#include <QSGTexture>
#include <rhi/qrhi.h>

#include <utility>

std::unique_ptr<TerminalStraightRgbaTexture>
TerminalStraightRgbaTexture::create(QQuickWindow *window, QImage straightRgba)
{
    if (window == nullptr || straightRgba.isNull()
        || straightRgba.format() != QImage::Format_RGBA8888) {
        return {};
    }
    QRhi *const rhi = window->rhi();
    if (rhi == nullptr) return {};
    QRhiTexture *const raw =
        rhi->newTexture(QRhiTexture::RGBA8, straightRgba.size());
    if (raw == nullptr) return {};
    if (!raw->create()) {
        delete raw;
        return {};
    }
    std::unique_ptr<QSGTexture> wrapper(window->createTextureFromRhiTexture(
        raw, QQuickWindow::TextureHasAlphaChannel));
    if (wrapper == nullptr) {
        delete raw;
        return {};
    }
    wrapper->setFiltering(QSGTexture::Linear);
    wrapper->setMipmapFiltering(QSGTexture::None);
    wrapper->setHorizontalWrapMode(QSGTexture::ClampToEdge);
    wrapper->setVerticalWrapMode(QSGTexture::ClampToEdge);
    return std::unique_ptr<TerminalStraightRgbaTexture>(
        new TerminalStraightRgbaTexture(raw, std::move(wrapper),
                                        std::move(straightRgba)));
}

TerminalStraightRgbaTexture::TerminalStraightRgbaTexture(
    QRhiTexture *raw, std::unique_ptr<QSGTexture> wrapper, QImage straightRgba)
    : raw_(raw)
    , wrapper_(std::move(wrapper))
    , straightRgba_(std::move(straightRgba))
{}

TerminalStraightRgbaTexture::~TerminalStraightRgbaTexture() = default;

QSGTexture *TerminalStraightRgbaTexture::sampledTexture() const noexcept
{
    return wrapper_.get();
}

bool TerminalStraightRgbaTexture::commitTextureOperations(
    QRhi *rhi, QRhiResourceUpdateBatch *resourceUpdates)
{
    wrapper_->commitTextureOperations(rhi, resourceUpdates);
    if (!uploadPending_ || resourceUpdates == nullptr) return false;

    const QByteArray pixels = QByteArray::fromRawData(
        reinterpret_cast<const char *>(straightRgba_.constBits()),
        straightRgba_.sizeInBytes());
    QRhiTextureSubresourceUploadDescription subresource(pixels);
    subresource.setDataStride(
        static_cast<quint32>(straightRgba_.bytesPerLine()));
    subresource.setSourceSize(straightRgba_.size());
    resourceUpdates->uploadTexture(
        raw_,
        QRhiTextureUploadDescription{
            QRhiTextureUploadEntry{0, 0, subresource}});
    uploadPending_ = false;
    ++uploadCount_;
    return true;
}

quint64 TerminalStraightRgbaTexture::logicalBytes() const noexcept
{
    return static_cast<quint64>(straightRgba_.sizeInBytes());
}

quint64 TerminalStraightRgbaTexture::uploadCount() const noexcept
{
    return uploadCount_;
}
