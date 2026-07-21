#include "private_session_bus.h"
#include "single_instance_activation.h"

#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>
#include <QVariantMap>

#include <chrono>
#include <future>
#include <utility>

using namespace std::chrono_literals;

namespace {

QString uniqueServiceName()
{
    QString suffix = QUuid::createUuid()
                         .toString(QUuid::WithoutBraces)
                         .remove(u'-');
    return QStringLiteral("io.github.JingYenLoh.ghostty_qt.Test.t%1")
        .arg(suffix);
}

QString objectPath(const QString &service)
{
    return SingleInstanceActivation::objectPathForApplicationId(service);
}

QDBusMessage activationCall(
    const QString &service, QVariantMap platformData = {})
{
    QDBusMessage message
        = QDBusMessage::createMethodCall(service, objectPath(service),
            QString::fromLatin1(SingleInstanceActivation::InterfaceName),
            QStringLiteral("Activate"));
    message << platformData;
    return message;
}

class NeverReplyEndpoint final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Application")

public Q_SLOTS:
    Q_SCRIPTABLE void Activate(const QVariantMap &)
    {
        setDelayedReply(true);
    }
};

class AcceptEndpoint final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Application")

public:
    int calls = 0;
    QVariantMap platformData;

public Q_SLOTS:
    Q_SCRIPTABLE void Activate(const QVariantMap &data)
    {
        ++calls;
        platformData = data;
    }
};

class HandoffEndpoint final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Application")

public:
    HandoffEndpoint(QDBusConnection retiring,
                    QDBusConnection successor,
                    QString service)
        : retiring_(std::move(retiring))
        , successor_(std::move(successor))
        , service_(std::move(service))
    {
    }

    int calls = 0;
    bool handoffSucceeded = false;

public Q_SLOTS:
    Q_SCRIPTABLE void Activate(const QVariantMap &)
    {
        setDelayedReply(true);
        ++calls;
        handoffSucceeded = retiring_.unregisterService(service_)
            && successor_.registerService(service_);
        (void)retiring_.send(message().createErrorReply(
            QStringLiteral("org.freedesktop.DBus.Error.Failed"),
            QStringLiteral("owner retired")));
    }

private:
    QDBusConnection retiring_;
    QDBusConnection successor_;
    QString service_;
};

} // namespace

class SingleInstanceActivationTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void firstRegistrantBecomesPrimary();
    void secondaryActivatesPrimaryExactlyOnce();
    void secondaryCanExitWithoutActivatingPrimary();
    void activationWaitsUntilHandlerIsInstalled();
    void queuedActivationReportsHandlerFailure();
    void activationFollowsAnOwnerHandoff();
    void exportsStandardInterfaceAndRejectsUnsupportedMethods();
    void disconnectedBusFallsBackAndNoReplyFailsClosed();
};

void SingleInstanceActivationTest::firstRegistrantBecomesPrimary()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    SingleInstanceActivation activation(bus.server(), uniqueServiceName());
    const auto result = activation.start({
        .timeout = 1s,
        .existingInstanceAction =
            SingleInstanceActivation::ExistingInstanceAction::DoNotActivate,
    });
    QCOMPARE(result.role, SingleInstanceActivation::Role::Primary);
    QVERIFY(result.diagnostic.isEmpty());
    QVERIFY(activation.isPrimary());
    const auto repeated = activation.start({.timeout = 1s});
    QCOMPARE(repeated.role, SingleInstanceActivation::Role::Failed);
    QVERIFY(!repeated.diagnostic.isEmpty());
    QVERIFY(activation.isPrimary());
}

void SingleInstanceActivationTest::secondaryActivatesPrimaryExactlyOnce()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);
    int activations = 0;
    DesktopActivationContext observed;
    primary.setActivationHandler([&](DesktopActivationContext activation) {
        ++activations;
        observed = std::move(activation);
        return true;
    });

    auto future = std::async(std::launch::async, [&bus, service] {
        SingleInstanceActivation secondary(bus.client(), service);
        return secondary.start({
            .timeout = 2s,
            .activation = {
                .xdgActivationToken = QStringLiteral("forwarded-token"),
                .desktopStartupId = QStringLiteral("forwarded-startup"),
            },
        });
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms)
                                 == std::future_status::ready,
                             3000);
    const auto result = future.get();
    QCOMPARE(result.role,
             SingleInstanceActivation::Role::ActivatedExisting);
    QCOMPARE(activations, 1);
    QCOMPARE(observed.xdgActivationToken,
             QStringLiteral("forwarded-token"));
    QCOMPARE(observed.desktopStartupId,
             QStringLiteral("forwarded-startup"));
}

void SingleInstanceActivationTest::secondaryCanExitWithoutActivatingPrimary()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);
    int activations = 0;
    primary.setActivationHandler([&activations](DesktopActivationContext) {
        ++activations;
        return true;
    });

    SingleInstanceActivation secondary(bus.client(), service);
    const auto result = secondary.start({
        .timeout = 1s,
        .existingInstanceAction =
            SingleInstanceActivation::ExistingInstanceAction::DoNotActivate,
    });
    QCOMPARE(result.role, SingleInstanceActivation::Role::ExistingInstance);
    QVERIFY(result.diagnostic.isEmpty());
    QCOMPARE(activations, 0);
}

void SingleInstanceActivationTest::activationWaitsUntilHandlerIsInstalled()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    QDBusPendingCallWatcher firstCall(bus.client().asyncCall(
        activationCall(service, {
            {QStringLiteral("activation-token"),
             QStringLiteral("queued-token")},
        }), 2000));
    QDBusPendingCallWatcher secondCall(bus.client().asyncCall(
        activationCall(service, {
            {QStringLiteral("activation-token"),
             QStringLiteral("queued-token-two")},
        }), 2000));
    QTest::qWait(100);
    QVERIFY(!firstCall.isFinished());
    QVERIFY(!secondCall.isFinished());

    int activations = 0;
    QStringList observedTokens;
    primary.setActivationHandler([&](DesktopActivationContext activation) {
        ++activations;
        observedTokens.append(std::move(activation.xdgActivationToken));
        return true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(firstCall.isFinished(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(secondCall.isFinished(), 3000);
    QCOMPARE(firstCall.reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(secondCall.reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(activations, 2);
    QCOMPARE(observedTokens, QStringList({
        QStringLiteral("queued-token"),
        QStringLiteral("queued-token-two"),
    }));
}

void SingleInstanceActivationTest::queuedActivationReportsHandlerFailure()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);

    auto future = std::async(std::launch::async, [&bus, service] {
        SingleInstanceActivation secondary(bus.client(), service);
        return secondary.start({.timeout = 2s});
    });
    QTest::qWait(100);
    QCOMPARE(future.wait_for(0ms), std::future_status::timeout);

    int activations = 0;
    primary.setActivationHandler([&activations](DesktopActivationContext) {
        ++activations;
        return false;
    });
    QTRY_COMPARE_WITH_TIMEOUT(future.wait_for(0ms),
                              std::future_status::ready, 3000);
    QCOMPARE(future.get().role,
             SingleInstanceActivation::Role::Failed);
    QCOMPARE(activations, 1);
}

void SingleInstanceActivationTest::activationFollowsAnOwnerHandoff()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    AcceptEndpoint successor;
    QVERIFY(bus.client().registerObject(objectPath(service),
        QString::fromLatin1(SingleInstanceActivation::InterfaceName),
        &successor, QDBusConnection::ExportScriptableSlots));
    HandoffEndpoint retiring(bus.server(), bus.client(), service);
    QVERIFY(bus.server().registerObject(objectPath(service),
        QString::fromLatin1(SingleInstanceActivation::InterfaceName),
        &retiring, QDBusConnection::ExportScriptableSlots));
    QVERIFY(bus.server().registerService(service));

    const QDBusConnection thirdClient = bus.connectClient();
    QVERIFY2(thirdClient.isConnected(),
             qPrintable(thirdClient.lastError().message()));
    auto future = std::async(
        std::launch::async, [thirdClient, service] {
            SingleInstanceActivation activation(thirdClient, service);
            return activation.start({
                .timeout = 2s,
                .activation = {
                    .xdgActivationToken = QStringLiteral("handoff-token"),
                },
            });
        });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms)
                                 == std::future_status::ready,
                             3000);
    QCOMPARE(future.get().role,
             SingleInstanceActivation::Role::ActivatedExisting);
    QCOMPARE(retiring.calls, 1);
    QVERIFY(retiring.handoffSucceeded);
    QCOMPARE(successor.calls, 1);
    QCOMPARE(successor.platformData.value(QStringLiteral("activation-token"))
                 .toString(),
             QStringLiteral("handoff-token"));
}

void SingleInstanceActivationTest::exportsStandardInterfaceAndRejectsUnsupportedMethods()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);
    int activations = 0;
    DesktopActivationContext observed;
    primary.setActivationHandler([&](DesktopActivationContext activation) {
        ++activations;
        observed = std::move(activation);
        return true;
    });

    QCOMPARE(objectPath(QStringLiteral("org.example.app-debug")),
        QStringLiteral("/org/example/app_debug"));

    auto future = std::async(std::launch::async, [&bus, service] {
        QVariantMap platformData;
        platformData.insert(
            QStringLiteral("activation-token"),
            QStringLiteral("token"));
        platformData.insert(
            QStringLiteral("desktop-startup-id"), QStringLiteral("startup"));
        platformData.insert(QStringLiteral("unknown"), 42);
        return bus.client().call(
            activationCall(service, platformData), QDBus::Block, 1000);
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms)
                                 == std::future_status::ready,
                             2000);
    const QDBusMessage reply = future.get();
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(reply.arguments().isEmpty());
    QCOMPARE(activations, 1);
    QCOMPARE(observed.xdgActivationToken, QStringLiteral("token"));
    QCOMPARE(observed.desktopStartupId, QStringLiteral("startup"));

    auto malformed = std::async(std::launch::async, [&bus, service] {
        return bus.client().call(
            activationCall(service, {
                {QStringLiteral("activation-token"), 7},
                {QStringLiteral("desktop-startup-id"),
                 QByteArrayLiteral("not-a-string")},
            }),
            QDBus::Block, 1000);
    });
    QTRY_COMPARE_WITH_TIMEOUT(
        malformed.wait_for(0ms), std::future_status::ready, 2000);
    QCOMPARE(malformed.get().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(activations, 2);
    QVERIFY(observed.isEmpty());

    const auto unsupported
        = [&bus, &service](QString method, QList<QVariant> arguments) {
              return std::async(std::launch::async,
                  [&bus, &service, method = std::move(method),
                      arguments = std::move(arguments)] {
                      QDBusMessage message = QDBusMessage::createMethodCall(
                          service, objectPath(service),
                          QString::fromLatin1(
                              SingleInstanceActivation::InterfaceName),
                          method);
                      message.setArguments(arguments);
                      return bus.client().call(message, QDBus::Block, 1000);
                  });
          };

    auto open = unsupported(QStringLiteral("Open"),
        {QVariant::fromValue(
             QStringList{QStringLiteral("file:tmp/a")}),
         QVariant::fromValue(QVariantMap{})});
    QTRY_COMPARE_WITH_TIMEOUT(
        open.wait_for(0ms), std::future_status::ready, 2000);
    QCOMPARE(QDBusError(open.get()).type(), QDBusError::NotSupported);

    auto action = unsupported(QStringLiteral("ActivateAction"),
        {QStringLiteral("new-window"),
         QVariant::fromValue(QVariantList{}),
         QVariant::fromValue(QVariantMap{})});
    QTRY_COMPARE_WITH_TIMEOUT(
        action.wait_for(0ms), std::future_status::ready, 2000);
    QCOMPARE(QDBusError(action.get()).type(), QDBusError::NotSupported);

    primary.release();
    QVERIFY(!primary.isPrimary());
    SingleInstanceActivation replacement(bus.client(), service);
    QCOMPARE(replacement.start({.timeout = 1s}).role,
             SingleInstanceActivation::Role::Primary);
}

void SingleInstanceActivationTest::disconnectedBusFallsBackAndNoReplyFailsClosed()
{
    const QString connectionName = QStringLiteral("ghostty_missing_bus_%1")
                                       .arg(QUuid::createUuid().toString(
                                           QUuid::WithoutBraces));
    const QString missingSocket = QDir::current().filePath(
        QStringLiteral("tmp/missing-session-bus-%1.sock")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    {
        SingleInstanceActivation disconnected(
            QDBusConnection::connectToBus(
                QStringLiteral("unix:path=%1").arg(missingSocket),
                connectionName),
            uniqueServiceName());
        const auto result = disconnected.start({.timeout = 50ms});
        QCOMPARE(result.role,
                 SingleInstanceActivation::Role::Independent);
        QVERIFY(!result.diagnostic.isEmpty());
    }
    QDBusConnection::disconnectFromBus(connectionName);

    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    NeverReplyEndpoint endpoint;
    QVERIFY(bus.server().registerObject(objectPath(service),
        QString::fromLatin1(SingleInstanceActivation::InterfaceName),
        &endpoint, QDBusConnection::ExportScriptableSlots));
    QVERIFY(bus.server().registerService(service));

    auto future = std::async(std::launch::async, [&bus, service] {
        SingleInstanceActivation secondary(bus.client(), service);
        return secondary.start({.timeout = 50ms});
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms)
                                 == std::future_status::ready,
                             2000);
    const auto result = future.get();
    QCOMPARE(result.role, SingleInstanceActivation::Role::Failed);
    QVERIFY(result.diagnostic.contains(
        QStringLiteral("possibly duplicate")));
}

QTEST_GUILESS_MAIN(SingleInstanceActivationTest)

#include "test_single_instance_activation.moc"
