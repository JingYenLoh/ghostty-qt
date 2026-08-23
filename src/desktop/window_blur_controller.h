#pragma once

#include <QObject>
#include <QPointer>
#include <QtTypes>

#include <functional>
#include <optional>

class QEvent;
class QWindow;

using WindowBlurApplier = std::function<bool(QWindow *, bool)>;

// Returns the compositor-specific implementation selected at build time. The
// callable is always present; builds without a supported backend are a
// deterministic no-op.
[[nodiscard]] WindowBlurApplier defaultWindowBlurApplier();

// Owns one window's compositor blur request without forcing its native Wayland
// surface into existence. The exact Ghostty value is retained even though KWin
// exposes only an enabled/disabled protocol.
class WindowBlurController final : public QObject {
    Q_OBJECT

public:
    explicit WindowBlurController(
        QWindow *window,
        WindowBlurApplier applier = defaultWindowBlurApplier());
    ~WindowBlurController() override;

    qint16 blur() const { return blur_; }
    bool enabled() const { return blur_ != 0; }
    void setBlur(qint16 blur);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void apply();

    QPointer<QWindow> window_;
    WindowBlurApplier applier_;
    std::optional<bool> applied_;
    qint16 blur_ = 0;
    bool surfaceCreated_ = false;
};
