#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QVariantMap>

class QWindow;

// Carries only the standard desktop presentation data that this frontend can
// apply. The D-Bus dictionary is untrusted; unknown keys and non-string values
// are deliberately discarded before the application controller sees them.
struct DesktopActivationContext final {
    QString xdgActivationToken;
    QString desktopStartupId;

    friend bool operator==(const DesktopActivationContext &,
                           const DesktopActivationContext &) = default;

    [[nodiscard]] static DesktopActivationContext fromPlatformData(
        const QVariantMap &platformData);
    // Capture and clear one-shot launcher state before any terminal or helper
    // child can inherit it.
    [[nodiscard]] static DesktopActivationContext takeFromEnvironment();
    [[nodiscard]] QVariantMap toPlatformData() const;
    [[nodiscard]] bool isEmpty() const noexcept
    {
        return xdgActivationToken.isEmpty() && desktopStartupId.isEmpty();
    }
};

// Presents one new primary window. Qt Wayland consumes the standard launcher
// variables synchronously while showing the window; their scoped projection
// avoids process-wide stale state and keeps the typed context above D-Bus.
void showWindowWithActivation(QWindow &window,
                              const DesktopActivationContext &activation);

// Session workers run outside the GUI thread. During presentation they reuse a
// clean pre-projection snapshot because Qt Wayland may itself mutate the
// process environment from showWindowWithActivation. Outside presentation the
// same state lock serializes a fresh snapshot against projection setup/cleanup.
[[nodiscard]] QProcessEnvironment sanitizedChildEnvironment();
