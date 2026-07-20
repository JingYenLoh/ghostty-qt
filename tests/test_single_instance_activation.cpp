#include "private_session_bus.h"
#include "single_instance_activation.h"

#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>

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

class NeverReplyEndpoint final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface",
                "io.github.JingYenLoh.ghostty_qt.Application1")

public Q_SLOTS:
    Q_SCRIPTABLE bool Activate(quint32)
    {
        setDelayedReply(true);
        return false;
    }
};

class AcceptEndpoint final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface",
                "io.github.JingYenLoh.ghostty_qt.Application1")

public:
    int calls = 0;

public Q_SLOTS:
    Q_SCRIPTABLE bool Activate(quint32 protocolVersion)
    {
        ++calls;
        return protocolVersion == SingleInstanceActivation::ProtocolVersion;
    }
};

class HandoffEndpoint final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface",
                "io.github.JingYenLoh.ghostty_qt.Application1")

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
    Q_SCRIPTABLE bool Activate(quint32)
    {
        ++calls;
        handoffSucceeded = retiring_.unregisterService(service_)
            && successor_.registerService(service_);
        return false;
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
    void activationWaitsUntilHandlerIsInstalled();
    void queuedActivationReportsHandlerFailure();
    void activationFollowsAnOwnerHandoff();
    void rejectsUnknownProtocolAndReleasesOwnership();
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
    const auto result = activation.start(1s);
    QCOMPARE(result.role, SingleInstanceActivation::Role::Primary);
    QVERIFY(result.diagnostic.isEmpty());
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
    QCOMPARE(primary.start(1s).role,
             SingleInstanceActivation::Role::Primary);
    int activations = 0;
    primary.setActivationHandler([&activations] {
        ++activations;
        return true;
    });

    auto future = std::async(std::launch::async, [&bus, service] {
        SingleInstanceActivation secondary(bus.client(), service);
        return secondary.start(2s);
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms)
                                 == std::future_status::ready,
                             3000);
    const auto result = future.get();
    QCOMPARE(result.role,
             SingleInstanceActivation::Role::ActivatedExisting);
    QCOMPARE(activations, 1);
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
    QCOMPARE(primary.start(1s).role,
             SingleInstanceActivation::Role::Primary);

    auto future = std::async(std::launch::async, [&bus, service] {
        SingleInstanceActivation secondary(bus.client(), service);
        return secondary.start(2s);
    });
    QTest::qWait(100);
    QCOMPARE(future.wait_for(0ms), std::future_status::timeout);

    int activations = 0;
    primary.setActivationHandler([&activations] {
        ++activations;
        return true;
    });
    QTRY_COMPARE_WITH_TIMEOUT(future.wait_for(0ms),
                              std::future_status::ready, 3000);
    QCOMPARE(future.get().role,
             SingleInstanceActivation::Role::ActivatedExisting);
    QCOMPARE(activations, 1);
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
    QCOMPARE(primary.start(1s).role,
             SingleInstanceActivation::Role::Primary);

    auto future = std::async(std::launch::async, [&bus, service] {
        SingleInstanceActivation secondary(bus.client(), service);
        return secondary.start(2s);
    });
    QTest::qWait(100);
    QCOMPARE(future.wait_for(0ms), std::future_status::timeout);

    int activations = 0;
    primary.setActivationHandler([&activations] {
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
    QVERIFY(bus.client().registerObject(
        QString::fromLatin1(SingleInstanceActivation::ObjectPath),
        QString::fromLatin1(SingleInstanceActivation::InterfaceName),
        &successor, QDBusConnection::ExportScriptableSlots));
    HandoffEndpoint retiring(bus.server(), bus.client(), service);
    QVERIFY(bus.server().registerObject(
        QString::fromLatin1(SingleInstanceActivation::ObjectPath),
        QString::fromLatin1(SingleInstanceActivation::InterfaceName),
        &retiring, QDBusConnection::ExportScriptableSlots));
    QVERIFY(bus.server().registerService(service));

    const QDBusConnection thirdClient = bus.connectClient();
    QVERIFY2(thirdClient.isConnected(),
             qPrintable(thirdClient.lastError().message()));
    auto future = std::async(
        std::launch::async, [thirdClient, service] {
            SingleInstanceActivation activation(thirdClient, service);
            return activation.start(2s);
        });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms)
                                 == std::future_status::ready,
                             3000);
    QCOMPARE(future.get().role,
             SingleInstanceActivation::Role::ActivatedExisting);
    QCOMPARE(retiring.calls, 1);
    QVERIFY(retiring.handoffSucceeded);
    QCOMPARE(successor.calls, 1);
}

void SingleInstanceActivationTest::rejectsUnknownProtocolAndReleasesOwnership()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    const QString service = uniqueServiceName();
    SingleInstanceActivation primary(bus.server(), service);
    QCOMPARE(primary.start(1s).role,
             SingleInstanceActivation::Role::Primary);
    int activations = 0;
    primary.setActivationHandler([&activations] {
        ++activations;
        return true;
    });

    auto future = std::async(std::launch::async, [&bus, service] {
        QDBusMessage message = QDBusMessage::createMethodCall(
            service,
            QString::fromLatin1(SingleInstanceActivation::ObjectPath),
            QString::fromLatin1(SingleInstanceActivation::InterfaceName),
            QStringLiteral("Activate"));
        message << quint32(SingleInstanceActivation::ProtocolVersion + 1);
        return QDBusReply<bool>(
            bus.client().call(message, QDBus::Block, 1000));
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms)
                                 == std::future_status::ready,
                             2000);
    const QDBusReply<bool> reply = future.get();
    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
    QCOMPARE(activations, 0);

    primary.release();
    QVERIFY(!primary.isPrimary());
    SingleInstanceActivation replacement(bus.client(), service);
    QCOMPARE(replacement.start(1s).role,
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
        const auto result = disconnected.start(50ms);
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
    QVERIFY(bus.server().registerObject(
        QString::fromLatin1(SingleInstanceActivation::ObjectPath),
        QString::fromLatin1(SingleInstanceActivation::InterfaceName),
        &endpoint, QDBusConnection::ExportScriptableSlots));
    QVERIFY(bus.server().registerService(service));

    auto future = std::async(std::launch::async, [&bus, service] {
        SingleInstanceActivation secondary(bus.client(), service);
        return secondary.start(50ms);
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
