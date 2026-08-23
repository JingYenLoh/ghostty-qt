#include "desktop/window_blur_controller.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QWindow>

#if GHOSTTY_QT_HAVE_KWINDOWEFFECTS
#include <KWindowEffects>
#endif

#include <utility>

WindowBlurApplier defaultWindowBlurApplier()
{
    return [](QWindow *window, bool enabled) {
        if (window == nullptr
            || QGuiApplication::platformName()
                != QLatin1StringView("wayland")) {
            return true;
        }

#if GHOSTTY_QT_HAVE_KWINDOWEFFECTS
        if (enabled
            && !KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind)) {
            // Do not cache compositor availability. A later synchronization
            // should retry after KWin enables or announces the effect.
            return false;
        }
        KWindowEffects::enableBlurBehind(window, enabled);
#else
        Q_UNUSED(enabled)
#endif
        return true;
    };
}

WindowBlurController::WindowBlurController(QWindow *window,
                                           WindowBlurApplier applier)
    : window_(window)
    , applier_(std::move(applier))
{
    if (window_ != nullptr) window_->installEventFilter(this);
}

WindowBlurController::~WindowBlurController()
{
    if (window_ != nullptr) window_->removeEventFilter(this);
}

void WindowBlurController::setBlur(qint16 blur)
{
    blur_ = blur;
    if (surfaceCreated_) apply();
}

bool WindowBlurController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == window_ && event->type() == QEvent::PlatformSurface) {
        const auto *const surfaceEvent =
            static_cast<QPlatformSurfaceEvent *>(event);
        switch (surfaceEvent->surfaceEventType()) {
        case QPlatformSurfaceEvent::SurfaceCreated:
            surfaceCreated_ = true;
            applied_.reset();
            apply();
            break;
        case QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed:
            surfaceCreated_ = false;
            applied_.reset();
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void WindowBlurController::apply()
{
    const bool requested = enabled();
    if (window_ == nullptr || !surfaceCreated_
        || (applied_.has_value() && *applied_ == requested) || !applier_) {
        return;
    }

    if (applier_(window_, requested)) {
        applied_ = requested;
    } else {
        applied_.reset();
    }
}
