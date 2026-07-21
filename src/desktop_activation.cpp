#include "desktop_activation.h"

#include <QMetaType>
#include <QMutex>
#include <QMutexLocker>
#include <QWindow>

#include <cstddef>

namespace {

constexpr auto XdgActivationTokenEnvironment = "XDG_ACTIVATION_TOKEN";
constexpr auto DesktopStartupIdEnvironment = "DESKTOP_STARTUP_ID";

QString exactString(const QVariantMap &platformData, const QString &key)
{
    const auto value = platformData.constFind(key);
    if (value == platformData.cend()
        || value->metaType() != QMetaType::fromType<QString>()) {
        return {};
    }
    const QString result = value->toString();
    return result.contains(QChar::Null) ? QString{} : result;
}

QProcessEnvironment sanitizedSystemEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("XDG_ACTIVATION_TOKEN"));
    environment.remove(QStringLiteral("DESKTOP_STARTUP_ID"));
    return environment;
}

struct ActivationEnvironmentState final {
    QMutex mutex;
    QProcessEnvironment childEnvironment;
    std::size_t presentationDepth = 0;
};

ActivationEnvironmentState &environmentState()
{
    static ActivationEnvironmentState state;
    return state;
}

QString takeEnvironmentVariable(const char *name)
{
    QString value = qEnvironmentVariable(name);
    (void)qunsetenv(name);
    return value;
}

void setEnvironmentVariable(const char *name, const QString &value)
{
    // Both variables are one-shot capabilities. Never preserve a stale value
    // outside the presentation call, even if projection fails.
    (void)qunsetenv(name);
    if (value.isEmpty()) return;
    const QByteArray encoded = value.toUtf8();
    if (!encoded.contains('\0')) (void)qputenv(name, encoded);
}

void clearActivationEnvironment()
{
    (void)qunsetenv(XdgActivationTokenEnvironment);
    (void)qunsetenv(DesktopStartupIdEnvironment);
}

void applyActivationEnvironment(const DesktopActivationContext &activation)
{
    setEnvironmentVariable(
        XdgActivationTokenEnvironment, activation.xdgActivationToken);
    setEnvironmentVariable(
        DesktopStartupIdEnvironment, activation.desktopStartupId);
}

class ScopedActivationProjection final {
public:
    explicit ScopedActivationProjection(
        const DesktopActivationContext &activation)
    {
        ActivationEnvironmentState &state = environmentState();
        const QMutexLocker lock(&state.mutex);
        if (state.presentationDepth == 0) {
            // Qt Wayland may unset the projected token from inside show().
            // Workers therefore reuse this clean snapshot until presentation
            // completes instead of walking process-global environ concurrently.
            state.childEnvironment = sanitizedSystemEnvironment();
        }
        ++state.presentationDepth;
        applyActivationEnvironment(activation);
    }

    ~ScopedActivationProjection()
    {
        ActivationEnvironmentState &state = environmentState();
        const QMutexLocker lock(&state.mutex);
        Q_ASSERT(state.presentationDepth > 0);
        --state.presentationDepth;
        // Presentation data is one-shot. Never restore an outer or stale
        // capability after Qt has had an opportunity to consume it.
        clearActivationEnvironment();
    }

    Q_DISABLE_COPY_MOVE(ScopedActivationProjection)
};

} // namespace

DesktopActivationContext DesktopActivationContext::fromPlatformData(
    const QVariantMap &platformData)
{
    return {
        .xdgActivationToken = exactString(
            platformData, QStringLiteral("activation-token")),
        .desktopStartupId = exactString(
            platformData, QStringLiteral("desktop-startup-id")),
    };
}

DesktopActivationContext DesktopActivationContext::takeFromEnvironment()
{
    ActivationEnvironmentState &state = environmentState();
    const QMutexLocker lock(&state.mutex);
    Q_ASSERT(state.presentationDepth == 0);
    return {
        .xdgActivationToken = takeEnvironmentVariable(
            XdgActivationTokenEnvironment),
        .desktopStartupId = takeEnvironmentVariable(
            DesktopStartupIdEnvironment),
    };
}

QVariantMap DesktopActivationContext::toPlatformData() const
{
    QVariantMap result;
    if (!xdgActivationToken.isEmpty()) {
        result.insert(
            QStringLiteral("activation-token"), xdgActivationToken);
    }
    if (!desktopStartupId.isEmpty()) {
        result.insert(
            QStringLiteral("desktop-startup-id"), desktopStartupId);
    }
    return result;
}

void showWindowWithActivation(QWindow &window,
                              const DesktopActivationContext &activation,
                              WindowPresentationMode mode)
{
    const ScopedActivationProjection projection(activation);

    // Qt Wayland applies XDG_ACTIVATION_TOKEN while the requested visibility
    // makes the window visible. Keep this as the sole presentation operation:
    // show() would reset a maximized/fullscreen state prepared while hidden,
    // while a second request would consume or replace the transferred token.
    switch (mode) {
    case WindowPresentationMode::Windowed:
        window.setVisibility(QWindow::Windowed);
        return;
    case WindowPresentationMode::Maximized:
        window.setVisibility(QWindow::Maximized);
        return;
    case WindowPresentationMode::Fullscreen:
        window.setVisibility(QWindow::FullScreen);
        return;
    }
}

QProcessEnvironment sanitizedChildEnvironment()
{
    ActivationEnvironmentState &state = environmentState();
    const QMutexLocker lock(&state.mutex);
    if (state.presentationDepth == 0) {
        state.childEnvironment = sanitizedSystemEnvironment();
    }
    return state.childEnvironment;
}
