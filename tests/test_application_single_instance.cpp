#include "private_session_bus.h"

#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include <algorithm>

#ifndef GHOSTTY_QT_TEST_CONFIG_ENABLED
#define GHOSTTY_QT_TEST_CONFIG_ENABLED 0
#endif

namespace {

bool waitForMarker(QProcess &process, QByteArray &output,
                   QByteArrayView marker, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMilliseconds) {
        output += process.readAllStandardOutput();
        if (output.contains(marker)) return true;
        if (process.state() == QProcess::NotRunning) break;
        const int remaining = timeoutMilliseconds
            - static_cast<int>(timer.elapsed());
        (void) process.waitForReadyRead(std::clamp(remaining, 1, 100));
    }
    output += process.readAllStandardOutput();
    return output.contains(marker);
}

QString processFailure(QProcess &process, const QByteArray &output)
{
    return QStringLiteral("state=%1 exit=%2 error=%3 stdout=%4 stderr=%5")
        .arg(process.state())
        .arg(process.exitCode())
        .arg(process.errorString(), QString::fromUtf8(output),
             QString::fromUtf8(process.readAllStandardError()));
}

bool writeGhosttyConfig(const QString &configHome,
                        const QByteArray &contents)
{
    const QString ghosttyDirectory =
        QDir(configHome).filePath(QStringLiteral("ghostty"));
    if (!QDir().mkpath(ghosttyDirectory)) return false;
    QFile config(QDir(ghosttyDirectory).filePath(
        QStringLiteral("config.ghostty")));
    return config.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && config.write(contents) == contents.size();
}

bool writeFrontendConfig(const QString &configHome, const QByteArray &contents)
{
    const QString frontendDirectory =
        QDir(configHome).filePath(QStringLiteral("ghostty-qt"));
    if (!QDir().mkpath(frontendDirectory)) return false;
    QFile config(QDir(frontendDirectory).filePath(QStringLiteral("config")));
    return config.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && config.write(contents) == contents.size();
}

QProcessEnvironment headlessApplicationEnvironment(const QString &configHome)
{
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("TERM_PROGRAM"));
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), configHome);
    environment.insert(QStringLiteral("GHOSTTY_QT_ALLOW_NON_WAYLAND"),
                       QStringLiteral("1"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"),
                       QStringLiteral("software"));
    return environment;
}

QProcessEnvironment applicationEnvironment(
    const PrivateSessionBus &bus, const QString &configHome)
{
    QProcessEnvironment environment
        = headlessApplicationEnvironment(configHome);
    environment.insert(
        QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), bus.address());
    return environment;
}

QString applicationId()
{
    return QStringLiteral(GHOSTTY_QT_TEST_APPLICATION_ID);
}

QString applicationObjectPath()
{
    QString path = QStringLiteral("/") + applicationId();
    path.replace(u'.', u'/');
    path.replace(u'-', u'_');
    return path;
}

class RecordingActivationEndpoint final : public QObject {
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
        Q_EMIT activated();
    }

Q_SIGNALS:
    void activated();
};

QDBusMessage activateApplication(QDBusConnection &connection)
{
    QDBusMessage request = QDBusMessage::createMethodCall(applicationId(),
        applicationObjectPath(), QStringLiteral("org.freedesktop.Application"),
        QStringLiteral("Activate"));
    request << QVariantMap{};
    return connection.call(request, QDBus::Block, 15'000);
}

bool writeActivationService(const QString &dataHome)
{
    const QString serviceDirectory
        = QDir(dataHome).filePath(QStringLiteral("dbus-1/services"));
    if (!QDir().mkpath(serviceDirectory)) return false;

    QString executable = QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE);
    executable.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    executable.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    const QByteArray contents =
        QStringLiteral(
            "[D-BUS Service]\n"
            "Name=%1\n"
            "Exec=\"%2\" --single-instance=true --initial-window=false\n")
            .arg(applicationId(), executable)
            .toUtf8();
    QFile service(QDir(serviceDirectory).filePath(
        applicationId() + QStringLiteral(".service")));
    return service.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && service.write(contents) == contents.size();
}

bool serviceHasOwner(PrivateSessionBus &bus)
{
    const QDBusReply<bool> registered
        = bus.client().interface()->isServiceRegistered(applicationId());
    return registered.isValid() && registered.value();
}

} // namespace

class ApplicationSingleInstanceTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void execFallbackForwardsLauncherPlatformData();
    void explicitZeroWindowHostSupportsStandardActivation();
    void dbusColdStartsZeroWindowHost();
    void warmHostAcceptsGhosttyCliApplicationActions_data();
    void warmHostAcceptsGhosttyCliApplicationActions();
    void ghosttyCliActionColdStartsService_data();
    void ghosttyCliActionColdStartsService();
#if GHOSTTY_QT_TEST_CONFIG_ENABLED
    void residentPrimaryIsReactivatedByBareSecondLaunch();
    void falseLauncherLeavesPrimaryAtZeroUntilTrueLauncherActivates();
#endif
};

void ApplicationSingleInstanceTest::execFallbackForwardsLauncherPlatformData()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir configHome(QDir::current().filePath(
        QStringLiteral("tmp/application-desktop-forwarding-XXXXXX")));
    QVERIFY(configHome.isValid());
    QVERIFY(writeGhosttyConfig(configHome.path(),
                               QByteArrayLiteral("initial-window = true\n")));
    QVERIFY(writeFrontendConfig(
        configHome.path(), QByteArrayLiteral("single-instance = false\n")));

    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));
    RecordingActivationEndpoint endpoint;
    QVERIFY(bus.server().registerObject(applicationObjectPath(),
        QStringLiteral("org.freedesktop.Application"), &endpoint,
        QDBusConnection::ExportScriptableSlots));
    QVERIFY(bus.server().registerService(applicationId()));

    QProcessEnvironment environment =
        applicationEnvironment(bus, configHome.path());
    environment.insert(QStringLiteral("XDG_ACTIVATION_TOKEN"),
                       QStringLiteral("fallback-token"));
    environment.insert(QStringLiteral("DESKTOP_STARTUP_ID"),
                       QStringLiteral("fallback-startup"));
    QProcess secondary;
    secondary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    secondary.setArguments({QStringLiteral("--single-instance=true")});
    secondary.setProcessEnvironment(environment);
    secondary.start();
    QVERIFY(secondary.waitForStarted(3000));
    const auto cleanup = qScopeGuard([&secondary] {
        if (secondary.state() == QProcess::NotRunning) return;
        secondary.kill();
        secondary.waitForFinished(3000);
    });

    QTRY_COMPARE_WITH_TIMEOUT(endpoint.calls, 1, 10'000);
    QCOMPARE(endpoint.platformData.value(
                 QStringLiteral("activation-token")).toString(),
             QStringLiteral("fallback-token"));
    QCOMPARE(endpoint.platformData.value(
                 QStringLiteral("desktop-startup-id")).toString(),
             QStringLiteral("fallback-startup"));
    QCOMPARE(endpoint.platformData.size(), 2);
    if (secondary.state() != QProcess::NotRunning) {
        QVERIFY2(secondary.waitForFinished(10'000),
                 qPrintable(processFailure(
                     secondary, secondary.readAllStandardOutput())));
    }
    QCOMPARE(secondary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(secondary.exitCode(), 0);
}

void ApplicationSingleInstanceTest::explicitZeroWindowHostSupportsStandardActivation()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir configHome(QDir::current().filePath(
        QStringLiteral("tmp/application-desktop-warm-XXXXXX")));
    QVERIFY(configHome.isValid());
    QVERIFY(writeGhosttyConfig(
        configHome.path(),
        QByteArrayLiteral("initial-window = true\n"
                          "confirm-close-surface = false\n")));
    QVERIFY(writeFrontendConfig(
        configHome.path(), QByteArrayLiteral("single-instance = false\n")));

    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    QProcessEnvironment environment
        = applicationEnvironment(bus, configHome.path());
    environment.insert(QStringLiteral("GHOSTTY_QT_TEST_DESKTOP_ACTIVATION"),
        QStringLiteral("1"));
    QProcess primary;
    primary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    primary.setArguments({QStringLiteral("--single-instance=true"),
                          QStringLiteral("--initial-window=false")});
    primary.setProcessEnvironment(environment);
    primary.start();
    QVERIFY(primary.waitForStarted(3000));
    const auto cleanup = qScopeGuard([&primary] {
        if (primary.state() == QProcess::NotRunning) return;
        primary.kill();
        primary.waitForFinished(3000);
    });

    QByteArray output;
    QVERIFY2(waitForMarker(primary, output,
                 QByteArrayView("GHOSTTY_QT_DESKTOP_ACTIVATION_READY"), 10'000),
        qPrintable(processFailure(primary, output)));
    QVERIFY(serviceHasOwner(bus));

    const QDBusMessage reply = activateApplication(bus.client());
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(reply.arguments().isEmpty());
    QVERIFY2(
        waitForMarker(primary, output,
            QByteArrayView("GHOSTTY_QT_DESKTOP_ACTIVATION_CREATED"), 10'000),
        qPrintable(processFailure(primary, output)));
    QVERIFY2(primary.waitForFinished(10'000),
        qPrintable(processFailure(primary, output)));
    QCOMPARE(primary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(primary.exitCode(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!serviceHasOwner(bus), 3000);
}

void ApplicationSingleInstanceTest::dbusColdStartsZeroWindowHost()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir activationRoot(QDir::current().filePath(
        QStringLiteral("tmp/application-desktop-cold-XXXXXX")));
    QVERIFY(activationRoot.isValid());
    const QDir root(activationRoot.path());
    const QString dataHome = root.filePath(QStringLiteral("data"));
    const QString configHome = root.filePath(QStringLiteral("config"));
    QVERIFY(writeActivationService(dataHome));
    QVERIFY(writeGhosttyConfig(
        configHome,
        QByteArrayLiteral("initial-window = true\n"
                          "confirm-close-surface = false\n")));
    QVERIFY(writeFrontendConfig(
        configHome, QByteArrayLiteral("single-instance = false\n")));

    QProcessEnvironment daemonEnvironment
        = headlessApplicationEnvironment(configHome);
    daemonEnvironment.insert(QStringLiteral("XDG_DATA_HOME"), dataHome);
    daemonEnvironment.insert(
        QStringLiteral("GHOSTTY_QT_TEST_DESKTOP_ACTIVATION"),
        QStringLiteral("1"));
    PrivateSessionBus bus;
    QVERIFY2(bus.start(daemonEnvironment), qPrintable(bus.errorString()));
    QVERIFY(!serviceHasOwner(bus));

    // The first call exercises real D-Bus service discovery and startup. The
    // test hook rejects an accidental bootstrap window, while the delayed
    // empty reply proves that the queued activation registered exactly one.
    for (int launch = 0; launch < 2; ++launch) {
        const QDBusMessage reply = activateApplication(bus.client());
        QVERIFY2(reply.type() == QDBusMessage::ReplyMessage,
            qPrintable(QDBusError(reply).message()));
        QVERIFY(reply.arguments().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!serviceHasOwner(bus), 10'000);
    }
}

void ApplicationSingleInstanceTest::
    warmHostAcceptsGhosttyCliApplicationActions_data()
{
    QTest::addColumn<QStringList>("arguments");
    QTest::newRow("new-window") << QStringList{
        QStringLiteral("+new-window"), QStringLiteral("--title=remote title"),
        QStringLiteral("-e"),          QStringLiteral("/bin/sleep"),
        QStringLiteral("1"),
    };
    QTest::newRow("toggle-quick-terminal") << QStringList{
        QStringLiteral("+toggle-quick-terminal"),
        QStringLiteral("--class=org.example.DeliberatelyIgnored"),
    };
}

void ApplicationSingleInstanceTest::
    warmHostAcceptsGhosttyCliApplicationActions()
{
    QFETCH(QStringList, arguments);
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir configHome(QDir::current().filePath(
        QStringLiteral("tmp/application-action-warm-XXXXXX")));
    QVERIFY(configHome.isValid());
    QVERIFY(writeGhosttyConfig(
        configHome.path(),
        QByteArrayLiteral("initial-window = true\n"
                          "confirm-close-surface = false\n")));
    QVERIFY(writeFrontendConfig(
        configHome.path(), QByteArrayLiteral("single-instance = false\n")));

    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));
    QProcessEnvironment environment =
        applicationEnvironment(bus, configHome.path());
    environment.insert(QStringLiteral("GHOSTTY_QT_TEST_DESKTOP_ACTIVATION"),
                       QStringLiteral("1"));

    QProcess primary;
    primary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    primary.setArguments({QStringLiteral("--single-instance=true"),
                          QStringLiteral("--initial-window=false")});
    primary.setProcessEnvironment(environment);
    primary.start();
    QVERIFY(primary.waitForStarted(3000));
    const auto cleanup = qScopeGuard([&primary] {
        if (primary.state() == QProcess::NotRunning) return;
        primary.kill();
        primary.waitForFinished(3000);
    });

    QByteArray output;
    QVERIFY2(waitForMarker(
                 primary, output,
                 QByteArrayView("GHOSTTY_QT_DESKTOP_ACTIVATION_READY"), 10'000),
             qPrintable(processFailure(primary, output)));
    QVERIFY(serviceHasOwner(bus));

    QProcess client;
    client.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    client.setArguments(arguments);
    client.setProcessEnvironment(environment);
    client.start();
    QVERIFY(client.waitForStarted(3000));
    QVERIFY2(
        client.waitForFinished(15'000),
        qPrintable(processFailure(client, client.readAllStandardOutput())));
    QCOMPARE(client.exitStatus(), QProcess::NormalExit);
    QCOMPARE(client.exitCode(), 0);

    QVERIFY2(
        waitForMarker(primary, output,
                      QByteArrayView("GHOSTTY_QT_DESKTOP_ACTIVATION_CREATED"),
                      10'000),
        qPrintable(processFailure(primary, output)));
    QVERIFY2(primary.waitForFinished(10'000),
             qPrintable(processFailure(primary, output)));
    QCOMPARE(primary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(primary.exitCode(), 0);
}

void ApplicationSingleInstanceTest::ghosttyCliActionColdStartsService_data()
{
    QTest::addColumn<QStringList>("arguments");
    QTest::newRow("new-window") << QStringList{
        QStringLiteral("+new-window"),
        QStringLiteral("--title=cold"),
        QStringLiteral("-e"),
        QStringLiteral("/bin/true"),
    };
    QTest::newRow("toggle-quick-terminal")
        << QStringList{QStringLiteral("+toggle-quick-terminal")};
}

void ApplicationSingleInstanceTest::ghosttyCliActionColdStartsService()
{
    QFETCH(QStringList, arguments);
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir activationRoot(QDir::current().filePath(
        QStringLiteral("tmp/application-action-cold-XXXXXX")));
    QVERIFY(activationRoot.isValid());
    const QDir root(activationRoot.path());
    const QString dataHome = root.filePath(QStringLiteral("data"));
    const QString configHome = root.filePath(QStringLiteral("config"));
    QVERIFY(writeActivationService(dataHome));
    QVERIFY(writeGhosttyConfig(
        configHome,
        QByteArrayLiteral("initial-window = true\n"
                          "confirm-close-surface = false\n")));
    QVERIFY(writeFrontendConfig(
        configHome, QByteArrayLiteral("single-instance = false\n")));

    QProcessEnvironment daemonEnvironment =
        headlessApplicationEnvironment(configHome);
    daemonEnvironment.insert(QStringLiteral("XDG_DATA_HOME"), dataHome);
    daemonEnvironment.insert(
        QStringLiteral("GHOSTTY_QT_TEST_DESKTOP_ACTIVATION"),
        QStringLiteral("1"));
    PrivateSessionBus bus;
    QVERIFY2(bus.start(daemonEnvironment), qPrintable(bus.errorString()));
    QVERIFY(!serviceHasOwner(bus));

    QProcess client;
    client.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    client.setArguments(arguments);
    client.setProcessEnvironment(applicationEnvironment(bus, configHome));
    client.start();
    QVERIFY(client.waitForStarted(3000));
    QVERIFY2(
        client.waitForFinished(15'000),
        qPrintable(processFailure(client, client.readAllStandardOutput())));
    QCOMPARE(client.exitStatus(), QProcess::NormalExit);
    QCOMPARE(client.exitCode(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!serviceHasOwner(bus), 10'000);
}

#if GHOSTTY_QT_TEST_CONFIG_ENABLED

void ApplicationSingleInstanceTest::residentPrimaryIsReactivatedByBareSecondLaunch()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir configHome(
        QDir::current().filePath(
            QStringLiteral("tmp/application-single-instance-XXXXXX")));
    QVERIFY(configHome.isValid());
    const QByteArray configContents =
        "initial-window = true\n"
        "quit-after-last-window-closed = false\n"
        "confirm-close-surface = false\n";
    QVERIFY(writeGhosttyConfig(configHome.path(), configContents));
    QVERIFY(writeFrontendConfig(configHome.path(),
                                QByteArrayLiteral("single-instance = true\n")));

    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    QProcessEnvironment environment = applicationEnvironment(
        bus, configHome.path());
    environment.insert(
        QStringLiteral("GHOSTTY_QT_TEST_APPLICATION_LIFETIME"),
        QStringLiteral("external-activation"));

    QProcess primary;
    primary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    primary.setProcessEnvironment(environment);
    primary.start();
    QVERIFY(primary.waitForStarted(3000));
    const auto cleanup = qScopeGuard([&primary] {
        if (primary.state() == QProcess::NotRunning) return;
        primary.kill();
        primary.waitForFinished(3000);
    });

    QByteArray primaryOutput;
    const QByteArrayView ready("GHOSTTY_QT_ACTIVATION_READY");
    QVERIFY2(waitForMarker(primary, primaryOutput, ready, 10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QCOMPARE(primary.state(), QProcess::Running);

    QProcess secondary;
    secondary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    secondary.setProcessEnvironment(environment);
    secondary.start();
    QVERIFY(secondary.waitForStarted(3000));
    QVERIFY2(secondary.waitForFinished(10'000),
             qPrintable(processFailure(
                 secondary, secondary.readAllStandardOutput())));
    QCOMPARE(secondary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(secondary.exitCode(), 0);

    const QByteArrayView accepted("GHOSTTY_QT_ACTIVATION_ACCEPTED");
    QVERIFY2(waitForMarker(primary, primaryOutput, accepted, 10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QVERIFY2(primary.waitForFinished(10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QCOMPARE(primary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(primary.exitCode(), 0);
}

void ApplicationSingleInstanceTest::falseLauncherLeavesPrimaryAtZeroUntilTrueLauncherActivates()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir falseConfigHome(
        QDir::current().filePath(
            QStringLiteral("tmp/application-initial-window-false-XXXXXX")));
    QTemporaryDir trueConfigHome(
        QDir::current().filePath(
            QStringLiteral("tmp/application-initial-window-true-XXXXXX")));
    QVERIFY(falseConfigHome.isValid());
    QVERIFY(trueConfigHome.isValid());
    QVERIFY(writeGhosttyConfig(
        falseConfigHome.path(),
        QByteArrayLiteral(
            "initial-window = false\n"
            "quit-after-last-window-closed = true\n"
            "confirm-close-surface = false\n")));
    QVERIFY(writeFrontendConfig(falseConfigHome.path(),
                                QByteArrayLiteral("single-instance = true\n")));
    QVERIFY(writeGhosttyConfig(
        trueConfigHome.path(),
        QByteArrayLiteral(
            "initial-window = true\n"
            "quit-after-last-window-closed = false\n"
            "confirm-close-surface = false\n")));
    QVERIFY(writeFrontendConfig(trueConfigHome.path(),
                                QByteArrayLiteral("single-instance = true\n")));

    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));

    QProcessEnvironment primaryEnvironment = applicationEnvironment(
        bus, falseConfigHome.path());
    primaryEnvironment.insert(QStringLiteral("GHOSTTY_QT_TEST_INITIAL_WINDOW"),
                              QStringLiteral("1"));
    QProcess primary;
    primary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    primary.setProcessEnvironment(primaryEnvironment);
    primary.start();
    QVERIFY(primary.waitForStarted(3000));
    const auto cleanup = qScopeGuard([&primary] {
        if (primary.state() == QProcess::NotRunning) return;
        primary.kill();
        primary.waitForFinished(3000);
    });

    QByteArray primaryOutput;
    const QByteArrayView ready("GHOSTTY_QT_INITIAL_WINDOW_READY");
    QVERIFY2(waitForMarker(primary, primaryOutput, ready, 10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QCOMPARE(primary.state(), QProcess::Running);

    QProcess falseSecondary;
    falseSecondary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    falseSecondary.setProcessEnvironment(applicationEnvironment(
        bus, falseConfigHome.path()));
    falseSecondary.start();
    QVERIFY(falseSecondary.waitForStarted(3000));
    QVERIFY2(falseSecondary.waitForFinished(10'000),
             qPrintable(processFailure(
                 falseSecondary, falseSecondary.readAllStandardOutput())));
    QCOMPARE(falseSecondary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(falseSecondary.exitCode(), 0);
    QTest::qWait(100);
    primaryOutput += primary.readAllStandardOutput();
    QVERIFY(!primaryOutput.contains("GHOSTTY_QT_INITIAL_WINDOW_CREATED"));
    QCOMPARE(primary.state(), QProcess::Running);

    QProcess trueSecondary;
    trueSecondary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    trueSecondary.setProcessEnvironment(applicationEnvironment(
        bus, trueConfigHome.path()));
    trueSecondary.start();
    QVERIFY(trueSecondary.waitForStarted(3000));
    QVERIFY2(trueSecondary.waitForFinished(10'000),
             qPrintable(processFailure(
                 trueSecondary, trueSecondary.readAllStandardOutput())));
    QCOMPARE(trueSecondary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(trueSecondary.exitCode(), 0);

    const QByteArrayView created("GHOSTTY_QT_INITIAL_WINDOW_CREATED");
    QVERIFY2(waitForMarker(primary, primaryOutput, created, 10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QVERIFY2(primary.waitForFinished(10'000),
             qPrintable(processFailure(primary, primaryOutput)));
    QCOMPARE(primary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(primary.exitCode(), 0);
}

#endif

QTEST_GUILESS_MAIN(ApplicationSingleInstanceTest)

#include "test_application_single_instance.moc"
