#pragma once

#include "config/ghostty_keybind_config.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>

#include <optional>

// A registration is deliberately action-string based. The portal only
// identifies the shortcut that fired; the application remains responsible for
// applying Ghostty's app/surface action scope when it handles the activation.
struct GhosttyGlobalShortcutRegistration {
    QString id;
    QString preferredTrigger;
    QString description;
    QString action;

    bool operator==(const GhosttyGlobalShortcutRegistration &) const = default;
};

enum class GhosttyGlobalShortcutDiagnosticCode {
    SequenceUnsupported,
    ActionChainUnsupported,
    CatchAllUnsupported,
    TriggerUnsupported,
    Collision,
};

struct GhosttyGlobalShortcutDiagnostic {
    GhosttyGlobalShortcutDiagnosticCode code =
        GhosttyGlobalShortcutDiagnosticCode::TriggerUnsupported;
    int rootBindingIndex = -1;
    QString message;

    bool operator==(const GhosttyGlobalShortcutDiagnostic &) const = default;
};

struct GhosttyGlobalShortcutRegistry {
    QVector<GhosttyGlobalShortcutRegistration> registrations;
    QVector<GhosttyGlobalShortcutDiagnostic> diagnostics;

    bool operator==(const GhosttyGlobalShortcutRegistry &) const = default;
};

// Translate a structured Ghostty trigger to the keysym spelling required by
// the XDG Global Shortcuts specification. std::nullopt means that the pinned
// GTK frontend would not be able to register the trigger either.
[[nodiscard]] std::optional<QString>
ghosttyXdgShortcutFromTrigger(const GhosttyKeybindTrigger &trigger);

// Build the authoritative portal registry. This intentionally mirrors the
// pinned GTK implementation's current eligibility rules: finalized root
// bindings only, global flag, one trigger, one action, and a translatable
// non-catch-all key. Output ordering and collision selection are stable.
[[nodiscard]] GhosttyGlobalShortcutRegistry
buildGhosttyGlobalShortcutRegistry(const GhosttyKeybindConfig &config);

class GhosttyGlobalShortcutPortal final : public QObject {
    Q_OBJECT

public:
    explicit GhosttyGlobalShortcutPortal(
        const QDBusConnection &connection = QDBusConnection::sessionBus(),
        QObject *parent = nullptr);
    ~GhosttyGlobalShortcutPortal() override;

    GhosttyGlobalShortcutPortal(const GhosttyGlobalShortcutPortal &) = delete;
    GhosttyGlobalShortcutPortal &
    operator=(const GhosttyGlobalShortcutPortal &) = delete;

    [[nodiscard]] const GhosttyGlobalShortcutRegistry &
    registry() const noexcept;
    [[nodiscard]] quint64 generation() const noexcept;
    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] QString sessionHandle() const;

    // An unchanged derived registry retains an active portal session. Changed
    // re-registration is atomic from the application's point of view: the old
    // session is closed before the new pure registry is installed and sent.
    void setKeybindConfig(const GhosttyKeybindConfig &config);
    void clear();

Q_SIGNALS:
    void registryChanged();
    void activeChanged(bool active);
    void warningOccurred(const QString &message);
    void shortcutActivated(const QString &action);

private Q_SLOTS:
    void onRequestResponse(uint response, const QVariantMap &results,
                           const QDBusMessage &message);
    void onActivated(const QDBusObjectPath &sessionHandle,
                     const QString &shortcutId, qulonglong timestamp,
                     const QVariantMap &options, const QDBusMessage &message);
    void onSessionClosed(const QVariantMap &details,
                         const QDBusMessage &message);

private:
    enum class RequestKind {
        CreateSession,
        BindShortcuts,
    };

    struct PendingRequest {
        RequestKind kind = RequestKind::CreateSession;
        quint64 generation = 0;
        QString canonicalPath;
    };

    void beginCreateSession();
    void beginBindShortcuts();
    void beginRequest(RequestKind kind, const QDBusMessage &methodCall,
                      const QString &expectedPath);
    [[nodiscard]] QString subscribeToResponse(RequestKind kind,
                                              const QString &requestToken);
    bool subscribeToResponsePath(const QString &path,
                                 const PendingRequest &request);
    void finishPendingRequest(const QString &canonicalPath);
    void closePortalState(bool notify);
    void setActive(bool active);
    void warn(const QString &message);

    QDBusConnection m_connection;
    GhosttyGlobalShortcutRegistry m_registry;
    QHash<QString, PendingRequest> m_pendingRequests;
    QHash<QString, QString> m_actionsById;
    QString m_sessionHandle;
    quint64 m_generation = 0;
    bool m_activationSubscribed = false;
    bool m_sessionClosedSubscribed = false;
    bool m_active = false;
};

Q_DECLARE_METATYPE(GhosttyGlobalShortcutDiagnosticCode)
Q_DECLARE_METATYPE(GhosttyGlobalShortcutRegistration)
Q_DECLARE_METATYPE(GhosttyGlobalShortcutDiagnostic)
Q_DECLARE_METATYPE(GhosttyGlobalShortcutRegistry)
