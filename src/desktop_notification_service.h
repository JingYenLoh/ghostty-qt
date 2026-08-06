#pragma once

#include "terminal_desktop_notification.h"
#include "workspace_ids.h"

#include <QDBusConnection>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>

#include <functional>
#include <optional>

struct DesktopNotificationDelivery {
    QString applicationName;
    QString iconName;
    QString title;
    QString body;
    QStringList actions;
    QVariantMap hints;
    quint32 replacesId = 0;
    int timeoutMilliseconds = -1;

    bool operator==(const DesktopNotificationDelivery &) const = default;
};

// Process-global policy and native Linux delivery for terminal-originated
// desktop notifications. The terminal callback remains byte-lifetime-safe and
// thread-agnostic; this GUI-thread owner applies Ghostty's throttling before
// crossing the freedesktop D-Bus boundary.
class DesktopNotificationService final : public QObject {
    Q_OBJECT

public:
    using Completion = std::function<void(std::optional<quint32>)>;
    using Presenter =
        std::function<void(const DesktopNotificationDelivery &, Completion)>;
    using Clock = std::function<qint64()>;

    explicit DesktopNotificationService(
        const QDBusConnection &connection = QDBusConnection::sessionBus(),
        QObject *parent = nullptr);
    DesktopNotificationService(Presenter presenter, Clock clock,
                               QObject *parent = nullptr);

    // False means the request was rate-limited. Native delivery is
    // asynchronous and non-fatal, so an accepted request returns true even if
    // the desktop notification service later rejects it.
    [[nodiscard]] bool show(const TerminalDesktopNotification &notification,
                            SurfaceTarget target);

Q_SIGNALS:
    void activationRequested(SurfaceTarget target);
    void warningOccurred(const QString &message);

public Q_SLOTS:
    // Public slots keep the freedesktop signal boundary independently
    // testable without exporting the service's internal maps.
    void handleActionInvoked(uint notificationId, const QString &action);
    void handleNotificationClosed(uint notificationId, uint reason);

private:
    struct PendingDelivery {
        QString body;
        SurfaceTarget target;
        quint64 sequence = 0;
    };

    void deliverOverDbus(const DesktopNotificationDelivery &delivery,
                         Completion completion);
    void finishDelivery(PendingDelivery pending,
                        std::optional<quint32> notificationId);
    void warnOnce(const QString &message);

    Presenter presenter_;
    Clock clock_;
    QDBusConnection connection_;
    QHash<quint32, SurfaceTarget> targetsByNotificationId_;
    QHash<quint32, QString> bodiesByNotificationId_;
    QHash<QString, quint32> notificationIdsByBody_;
    QHash<QString, quint64> latestSequenceByBody_;
    QString lastTitle_;
    QString lastBody_;
    std::optional<qint64> lastAcceptedMilliseconds_;
    quint64 nextSequence_ = 1;
    bool activationSignalsSubscribed_ = true;
    bool warned_ = false;
};

Q_DECLARE_METATYPE(DesktopNotificationDelivery)
