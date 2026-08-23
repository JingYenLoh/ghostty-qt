#pragma once

#include "terminal/model/terminal_backdrop_options.h"

#include <QColor>
#include <QImage>
#include <QObject>
#include <QRectF>
#include <QSize>

#include <expected>
#include <functional>
#include <memory>
#include <stop_token>
#include <utility>

struct TerminalBackgroundImageAsset {
    // Straight-alpha pixels are retained so the RHI path can interpolate
    // before shader premultiplication, matching Ghostty.
    QImage straightRgba;
    quint64 serial = 0;
};

[[nodiscard]] QRectF terminalBackgroundImagePlacement(
    const QRectF &viewport, const QSize &sourcePixels, qreal devicePixelRatio,
    TerminalBackgroundImageFit fit,
    TerminalBackgroundImagePosition position) noexcept;

// Pure reference composition matching Ghostty's background-image shader at
// source-pixel centers. The returned pixels are premultiplied and already
// include the terminal background.
[[nodiscard]] QImage
terminalCompositedBackgroundImage(const QImage &source,
                                  const QColor &opaqueBackground,
                                  quint8 backgroundAlpha, double imageOpacity);
[[nodiscard]] QImage
terminalCompositedBackgroundImage(const TerminalBackgroundImageAsset &asset,
                                  const QColor &opaqueBackground,
                                  quint8 backgroundAlpha, double imageOpacity);

using TerminalBackgroundImageResult =
    std::expected<std::shared_ptr<const TerminalBackgroundImageAsset>, QString>;
using TerminalBackgroundImageCallback =
    std::move_only_function<void(TerminalBackgroundImageResult)>;

struct TerminalBackgroundImageRequest {
    GhosttyConfigPath source;

    bool operator==(const TerminalBackgroundImageRequest &) const = default;
};

class TerminalBackgroundImageRequestHandle {
public:
    TerminalBackgroundImageRequestHandle() = default;
    TerminalBackgroundImageRequestHandle(
        TerminalBackgroundImageRequestHandle &&) noexcept = default;
    TerminalBackgroundImageRequestHandle &
    operator=(TerminalBackgroundImageRequestHandle &&other) noexcept
    {
        if (this != &other) {
            cancel();
            stopSource_ = std::move(other.stopSource_);
        }
        return *this;
    }
    TerminalBackgroundImageRequestHandle(
        const TerminalBackgroundImageRequestHandle &) = delete;
    TerminalBackgroundImageRequestHandle &
    operator=(const TerminalBackgroundImageRequestHandle &) = delete;
    ~TerminalBackgroundImageRequestHandle() { cancel(); }

    void cancel() noexcept
    {
        if (stopSource_.stop_possible()) {
            stopSource_.request_stop();
            stopSource_ = std::stop_source(std::nostopstate);
        }
    }

private:
    explicit TerminalBackgroundImageRequestHandle(std::stop_source stopSource)
        : stopSource_(std::move(stopSource))
    {}

    std::stop_source stopSource_{std::nostopstate};

    friend TerminalBackgroundImageRequestHandle
    requestTerminalBackgroundImage(const TerminalBackgroundImageRequest &,
                                   QObject *, TerminalBackgroundImageCallback);
    friend TerminalBackgroundImageRequestHandle
    requestTerminalBackgroundImageForTest(
        const TerminalBackgroundImageRequest &, QObject *,
        TerminalBackgroundImageCallback, std::move_only_function<void()>);
};

// Opening, descriptor inspection, decoding, and RGBA preparation are performed
// off the GUI/render threads. Identical opened-file identities are coalesced
// process-wide. A weak cache shares active images without retaining unused
// resources or hiding same-path replacements.
[[nodiscard]] TerminalBackgroundImageRequestHandle
requestTerminalBackgroundImage(const TerminalBackgroundImageRequest &request,
                               QObject *receiver,
                               TerminalBackgroundImageCallback callback);
