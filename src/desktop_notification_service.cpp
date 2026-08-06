#include "desktop_notification_service.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QPointer>

#include <chrono>
#include <utility>

namespace {

constexpr auto NotificationService = "org.freedesktop.Notifications";
constexpr auto NotificationPath = "/org/freedesktop/Notifications";
constexpr auto NotificationInterface = "org.freedesktop.Notifications";
constexpr auto DefaultAction = "default";
constexpr auto ApplicationName = "ghostty-qt";
constexpr auto DefaultTitle = "Ghostty";
constexpr qint64 GlobalIntervalMilliseconds = 1'000;
constexpr qint64 IdenticalIntervalMilliseconds = 5'000;
constexpr qsizetype MaximumTrackedNotifications = 256;

qint64 monotonicMilliseconds()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

DesktopNotificationService::DesktopNotificationService(
    const QDBusConnection &connection, QObject *parent)
    : QObject(parent)
    , clock_(monotonicMilliseconds)
    , connection_(connection)
{
    presenter_ = [this](const DesktopNotificationDelivery &delivery,
                        Completion completion) {
        deliverOverDbus(delivery, std::move(completion));
    };

    const bool actionConnected =
        connection_.connect(QString::fromLatin1(NotificationService),
                            QString::fromLatin1(NotificationPath),
                            QString::fromLatin1(NotificationInterface),
                            QStringLiteral("ActionInvoked"), this,
                            SLOT(handleActionInvoked(uint, QString)));
    const bool closedConnected =
        connection_.connect(QString::fromLatin1(NotificationService),
                            QString::fromLatin1(NotificationPath),
                            QString::fromLatin1(NotificationInterface),
                            QStringLiteral("NotificationClosed"), this,
                            SLOT(handleNotificationClosed(uint, uint)));
    activationSignalsSubscribed_ = actionConnected && closedConnected;
}

DesktopNotificationService::DesktopNotificationService(Presenter presenter,
                                                       Clock clock,
                                                       QObject *parent)
    : QObject(parent)
    , presenter_(std::move(presenter))
    , clock_(std::move(clock))
    , connection_(QStringLiteral("ghostty-qt-notification-test"))
{}

bool DesktopNotificationService::show(
    const TerminalDesktopNotification &notification, SurfaceTarget target)
{
    if (!target.isValid() || !presenter_ || !clock_) return false;

    // A missing session bus or notification host is irrelevant until a
    // terminal actually requests this optional feature. Deferring the warning
    // avoids noisy startup diagnostics in minimal sessions and headless UI
    // tests while still reporting that an accepted notification cannot route
    // a later click back to its pane.
    if (!activationSignalsSubscribed_) {
        warnOnce(QStringLiteral("Unable to subscribe to desktop notification "
                                "activation signals"));
    }

    const qint64 now = clock_();
    if (lastAcceptedMilliseconds_.has_value()) {
        const qint64 elapsed = now >= *lastAcceptedMilliseconds_
            ? now - *lastAcceptedMilliseconds_
            : 0;
        if (elapsed < GlobalIntervalMilliseconds) return false;
        if (notification.title == lastTitle_ && notification.body == lastBody_
            && elapsed < IdenticalIntervalMilliseconds) {
            return false;
        }
    }

    // Match Ghostty: an accepted request consumes the rate-limit slot even if
    // the native backend subsequently fails.
    lastAcceptedMilliseconds_ = now;
    lastTitle_ = notification.title;
    lastBody_ = notification.body;

    const quint64 sequence = nextSequence_++;
    latestSequenceByBody_.insert(notification.body, sequence);
    const PendingDelivery pending{
        .body = notification.body,
        .target = target,
        .sequence = sequence,
    };
    const DesktopNotificationDelivery delivery{
        .applicationName = QString::fromLatin1(ApplicationName),
        .iconName = QStringLiteral(GHOSTTY_QT_APPLICATION_ID),
        .title = notification.title.isEmpty()
            ? QString::fromLatin1(DefaultTitle)
            : notification.title,
        .body = notification.body,
        .actions = {QString::fromLatin1(DefaultAction), QStringLiteral("Open")},
        .hints = {{QStringLiteral("desktop-entry"),
                   QStringLiteral(GHOSTTY_QT_APPLICATION_ID)}},
        .replacesId = notificationIdsByBody_.value(notification.body, 0),
        .timeoutMilliseconds = -1,
    };

    const QPointer<DesktopNotificationService> guard(this);
    presenter_(delivery, [guard, pending](std::optional<quint32> id) {
        if (guard != nullptr) guard->finishDelivery(pending, id);
    });
    return true;
}

void DesktopNotificationService::handleActionInvoked(uint notificationId,
                                                     const QString &action)
{
    if (action != QString::fromLatin1(DefaultAction)) return;
    const auto found = targetsByNotificationId_.constFind(notificationId);
    if (found != targetsByNotificationId_.cend()) {
        Q_EMIT activationRequested(*found);
    }
}

void DesktopNotificationService::handleNotificationClosed(uint notificationId,
                                                          uint)
{
    targetsByNotificationId_.remove(notificationId);
    const auto found = bodiesByNotificationId_.constFind(notificationId);
    if (found == bodiesByNotificationId_.cend()) return;
    const QString body = *found;
    bodiesByNotificationId_.erase(found);
    if (notificationIdsByBody_.value(body, 0) == notificationId) {
        notificationIdsByBody_.remove(body);
    }
}

void DesktopNotificationService::deliverOverDbus(
    const DesktopNotificationDelivery &delivery, Completion completion)
{
    if (!connection_.isConnected()) {
        warnOnce(
            QStringLiteral("Desktop notification session bus is unavailable"));
        completion(std::nullopt);
        return;
    }

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(NotificationService),
        QString::fromLatin1(NotificationPath),
        QString::fromLatin1(NotificationInterface), QStringLiteral("Notify"));
    call << delivery.applicationName << delivery.replacesId << delivery.iconName
         << delivery.title << delivery.body << delivery.actions
         << delivery.hints << delivery.timeoutMilliseconds;

    auto *watcher =
        new QDBusPendingCallWatcher(connection_.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, completion = std::move(completion)](
                QDBusPendingCallWatcher *finished) mutable {
                const QDBusPendingReply<uint> reply = *finished;
                finished->deleteLater();
                if (reply.isError() || reply.value() == 0) {
                    warnOnce(QStringLiteral(
                                 "Desktop notification delivery failed: %1")
                                 .arg(reply.error().message()));
                    completion(std::nullopt);
                    return;
                }
                completion(reply.value());
            });
}

void DesktopNotificationService::finishDelivery(
    PendingDelivery pending, std::optional<quint32> notificationId)
{
    if (latestSequenceByBody_.value(pending.body, 0) != pending.sequence) {
        return;
    }
    latestSequenceByBody_.remove(pending.body);
    if (!notificationId.has_value() || *notificationId == 0) return;

    const quint32 oldId = notificationIdsByBody_.value(pending.body, 0);
    if (oldId != 0 && oldId != *notificationId) {
        targetsByNotificationId_.remove(oldId);
        bodiesByNotificationId_.remove(oldId);
    }
    const auto previousBody =
        bodiesByNotificationId_.constFind(*notificationId);
    if (previousBody != bodiesByNotificationId_.cend()
        && *previousBody != pending.body
        && notificationIdsByBody_.value(*previousBody, 0) == *notificationId) {
        notificationIdsByBody_.remove(*previousBody);
    }
    notificationIdsByBody_.insert(pending.body, *notificationId);
    targetsByNotificationId_.insert(*notificationId, pending.target);
    bodiesByNotificationId_.insert(*notificationId, pending.body);

    // A daemon that never emits NotificationClosed must not make terminal
    // output grow this process-global bookkeeping without bound.
    while (targetsByNotificationId_.size() > MaximumTrackedNotifications) {
        const auto victim = targetsByNotificationId_.cbegin();
        const quint32 victimId = victim.key();
        targetsByNotificationId_.erase(victim);
        const QString victimBody = bodiesByNotificationId_.take(victimId);
        if (notificationIdsByBody_.value(victimBody, 0) == victimId) {
            notificationIdsByBody_.remove(victimBody);
        }
    }
}

void DesktopNotificationService::warnOnce(const QString &message)
{
    if (warned_) return;
    warned_ = true;
    Q_EMIT warningOccurred(message);
}
