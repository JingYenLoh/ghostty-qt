#pragma once

#include "launch_options.h"

#include <QChronoTimer>
#include <QObject>
#include <QSet>

class QWindow;

// Owns process lifetime independently of any workspace. Primary windows close
// immediately after their workers approve shutdown; this GUI-thread object
// then applies Ghostty's global last-window policy and cancellable delay.
class ApplicationLifetimeController final : public QObject {
    Q_OBJECT

public:
    explicit ApplicationLifetimeController(QObject *parent = nullptr);

    void applyLaunchOptions(const LaunchOptions &options);
    bool registerWindow(QWindow *window);
    void lastWindowClosed();
    void requestQuitNow();

    [[nodiscard]] int registeredWindowCount() const
    {
        return static_cast<int>(registeredWindows_.size());
    }
    [[nodiscard]] bool hasOpenWindow() const { return hasOpenWindow_; }
    [[nodiscard]] bool quitPending() const { return quitTimer_.isActive(); }
    [[nodiscard]] bool hasRequestedQuit() const { return quitRequested_; }

Q_SIGNALS:
    void quitRequested();

private:
    void windowOpened();
    void reconcile();
    void cancelPendingQuit();

    QSet<QWindow *> registeredWindows_;
    QChronoTimer quitTimer_;
    bool quitAfterLastWindowClosed_ = true;
    std::optional<std::chrono::milliseconds> quitDelay_;
    bool sawVisibleWindow_ = false;
    bool hasOpenWindow_ = false;
    bool quitRequested_ = false;
};
