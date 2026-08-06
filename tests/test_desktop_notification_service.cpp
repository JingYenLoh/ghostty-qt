#include "desktop_notification_service.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <optional>
#include <utility>

class DesktopNotificationServiceTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void presentsNativeShapeAndDefaultTitle();
    void matchesGhosttySequentialRateLimits();
    void replacesBodyAndRoutesDefaultActivation();
    void ignoresStaleAsyncReplacementAndClosedNotifications();
    void defersUnavailableBusWarningUntilFirstDelivery();
};

void DesktopNotificationServiceTest::presentsNativeShapeAndDefaultTitle()
{
    qint64 now = 12;
    QVector<DesktopNotificationDelivery> deliveries;
    DesktopNotificationService service(
        [&deliveries](const DesktopNotificationDelivery &delivery,
                      DesktopNotificationService::Completion completion) {
            deliveries.append(delivery);
            completion(41);
        },
        [&now] { return now; });

    QVERIFY(service.show({.title = {}, .body = QStringLiteral("finished")},
                         {WindowId{3}, PaneId{7}}));
    QCOMPARE(deliveries.size(), 1);
    const DesktopNotificationDelivery &delivery = deliveries.front();
    QCOMPARE(delivery.applicationName, QStringLiteral("ghostty-qt"));
    QCOMPARE(delivery.iconName, QStringLiteral(GHOSTTY_QT_APPLICATION_ID));
    QCOMPARE(delivery.title, QStringLiteral("Ghostty"));
    QCOMPARE(delivery.body, QStringLiteral("finished"));
    QCOMPARE(delivery.actions,
             QStringList({QStringLiteral("default"), QStringLiteral("Open")}));
    QCOMPARE(delivery.hints.value(QStringLiteral("desktop-entry")).toString(),
             QStringLiteral(GHOSTTY_QT_APPLICATION_ID));
    QCOMPARE(delivery.replacesId, 0U);
    QCOMPARE(delivery.timeoutMilliseconds, -1);
}

void DesktopNotificationServiceTest::matchesGhosttySequentialRateLimits()
{
    qint64 now = 0;
    QVector<DesktopNotificationDelivery> deliveries;
    DesktopNotificationService service(
        [&deliveries](const DesktopNotificationDelivery &delivery,
                      DesktopNotificationService::Completion completion) {
            deliveries.append(delivery);
            completion(static_cast<quint32>(deliveries.size()));
        },
        [&now] { return now; });
    const SurfaceTarget target{WindowId{1}, PaneId{2}};
    const TerminalDesktopNotification a{QStringLiteral("A"),
                                        QStringLiteral("body-a")};
    const TerminalDesktopNotification b{QStringLiteral("B"),
                                        QStringLiteral("body-b")};

    QVERIFY(service.show(a, target));
    now = 999;
    QVERIFY(!service.show(b, target));
    now = 1'000;
    QVERIFY(service.show(b, target));
    // Ghostty compares the digest only with the last accepted request, rather
    // than retaining a five-second cache for every historical payload.
    now = 2'000;
    QVERIFY(service.show(a, target));
    now = 3'000;
    QVERIFY(!service.show(a, target));
    now = 7'000;
    QVERIFY(service.show(a, target));
    QCOMPARE(deliveries.size(), 4);

    QVERIFY(!service.show(a, {}));
    QCOMPARE(deliveries.size(), 4);
}

void DesktopNotificationServiceTest::replacesBodyAndRoutesDefaultActivation()
{
    qint64 now = 0;
    QVector<DesktopNotificationDelivery> deliveries;
    quint32 nextId = 19;
    DesktopNotificationService service(
        [&deliveries, &nextId](const DesktopNotificationDelivery &delivery,
                               DesktopNotificationService::Completion done) {
            deliveries.append(delivery);
            done(nextId);
        },
        [&now] { return now; });
    QSignalSpy activationSpy(&service,
                             &DesktopNotificationService::activationRequested);

    const TerminalDesktopNotification notification{QStringLiteral("Build"),
                                                   QStringLiteral("complete")};
    const SurfaceTarget first{WindowId{1}, PaneId{3}};
    QVERIFY(service.show(notification, first));
    QCOMPARE(deliveries.back().replacesId, 0U);

    service.handleActionInvoked(19, QStringLiteral("secondary"));
    QCOMPARE(activationSpy.size(), 0);
    service.handleActionInvoked(19, QStringLiteral("default"));
    QCOMPARE(activationSpy.size(), 1);
    QCOMPARE(qvariant_cast<SurfaceTarget>(activationSpy.takeFirst().front()),
             first);

    now = 5'000;
    const SurfaceTarget second{WindowId{4}, PaneId{8}};
    QVERIFY(service.show(notification, second));
    QCOMPARE(deliveries.back().replacesId, 19U);
    service.handleActionInvoked(19, QStringLiteral("default"));
    QCOMPARE(activationSpy.size(), 1);
    QCOMPARE(qvariant_cast<SurfaceTarget>(activationSpy.takeFirst().front()),
             second);
}

void DesktopNotificationServiceTest::
    ignoresStaleAsyncReplacementAndClosedNotifications()
{
    qint64 now = 0;
    QVector<DesktopNotificationService::Completion> completions;
    DesktopNotificationService service(
        [&completions](const DesktopNotificationDelivery &,
                       DesktopNotificationService::Completion completion) {
            completions.append(std::move(completion));
        },
        [&now] { return now; });
    QSignalSpy activationSpy(&service,
                             &DesktopNotificationService::activationRequested);
    const TerminalDesktopNotification notification{QStringLiteral("title"),
                                                   QStringLiteral("same-body")};

    QVERIFY(service.show(notification, {WindowId{1}, PaneId{1}}));
    now = 5'000;
    QVERIFY(service.show(notification, {WindowId{2}, PaneId{2}}));
    QCOMPARE(completions.size(), 2);
    completions[1](27);
    completions[0](26);

    service.handleActionInvoked(26, QStringLiteral("default"));
    QCOMPARE(activationSpy.size(), 0);
    service.handleActionInvoked(27, QStringLiteral("default"));
    QCOMPARE(activationSpy.size(), 1);
    QCOMPARE(qvariant_cast<SurfaceTarget>(activationSpy.takeFirst().front()),
             (SurfaceTarget{WindowId{2}, PaneId{2}}));

    service.handleNotificationClosed(27, 2);
    service.handleActionInvoked(27, QStringLiteral("default"));
    QCOMPARE(activationSpy.size(), 0);
}

void DesktopNotificationServiceTest::
    defersUnavailableBusWarningUntilFirstDelivery()
{
    DesktopNotificationService service(
        QDBusConnection(QStringLiteral("ghostty-qt-missing-notification-bus")));
    QSignalSpy warningSpy(&service,
                          &DesktopNotificationService::warningOccurred);

    QCoreApplication::processEvents();
    QCOMPARE(warningSpy.size(), 0);

    QVERIFY(service.show(
        {.title = QStringLiteral("Build"), .body = QStringLiteral("Complete")},
        {WindowId{1}, PaneId{2}}));
    QCOMPARE(warningSpy.size(), 1);

    QCoreApplication::processEvents();
    QCOMPARE(warningSpy.size(), 1);
}

QTEST_GUILESS_MAIN(DesktopNotificationServiceTest)

#include "test_desktop_notification_service.moc"
