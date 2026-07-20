#include "application_lifetime.h"

#include <QWindow>

ApplicationLifetimeController::ApplicationLifetimeController(QObject *parent)
    : QObject(parent)
{
    quitTimer_.setSingleShot(true);
    connect(&quitTimer_, &QChronoTimer::timeout, this, [this] {
        // Revalidate all state at delivery time. stop()/start() replaces Qt's
        // timer ID, while these guards also make a queued stale delivery inert
        // after a window opens or live configuration disables the policy.
        if (!quitAfterLastWindowClosed_ || hasOpenWindow_
            || !sawVisibleWindow_ || quitRequested_) {
            return;
        }
        requestQuitNow();
    });
}

void ApplicationLifetimeController::applyLaunchOptions(
    const LaunchOptions &options)
{
    if (quitAfterLastWindowClosed_ == options.quitAfterLastWindowClosed
        && quitDelay_ == options.quitAfterLastWindowClosedDelay) {
        return;
    }

    quitAfterLastWindowClosed_ = options.quitAfterLastWindowClosed;
    quitDelay_ = options.quitAfterLastWindowClosedDelay;
    reconcile();
}

bool ApplicationLifetimeController::registerWindow(QWindow *window)
{
    // Transient dialogs do not represent terminal application windows and
    // must not postpone the last-window policy.
    if (window == nullptr || window->transientParent() != nullptr
        || registeredWindows_.contains(window)) {
        return false;
    }

    registeredWindows_.insert(window);
    connect(window, &QWindow::visibleChanged, this,
            [this](bool visible) {
                if (visible) windowOpened();
            });
    connect(window, &QObject::destroyed, this, [this, window] {
        registeredWindows_.remove(window);
    });
    if (window->isVisible()) windowOpened();
    return true;
}

void ApplicationLifetimeController::lastWindowClosed()
{
    if (quitRequested_) return;
    hasOpenWindow_ = false;
    reconcile();
}

void ApplicationLifetimeController::requestQuitNow()
{
    if (quitRequested_) return;
    cancelPendingQuit();
    quitRequested_ = true;
    Q_EMIT quitRequested();
}

void ApplicationLifetimeController::windowOpened()
{
    if (quitRequested_) return;
    sawVisibleWindow_ = true;
    hasOpenWindow_ = true;
    cancelPendingQuit();
}

void ApplicationLifetimeController::reconcile()
{
    cancelPendingQuit();
    if (quitRequested_ || !sawVisibleWindow_ || hasOpenWindow_
        || !quitAfterLastWindowClosed_) {
        return;
    }

    quitTimer_.setInterval(quitDelay_.value_or(std::chrono::milliseconds::zero()));
    quitTimer_.start();
}

void ApplicationLifetimeController::cancelPendingQuit()
{
    if (quitTimer_.isActive()) quitTimer_.stop();
}
