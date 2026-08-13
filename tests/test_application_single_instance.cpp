#include "private_session_bus.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVirtualObject>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

bool writeDesktopEntry(const QString &dataHome, const QString &applicationId)
{
    const QString applicationsDirectory =
        QDir(dataHome).filePath(QStringLiteral("applications"));
    if (!QDir().mkpath(applicationsDirectory)) return false;
    const QByteArray contents =
        QByteArrayLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=Ghostty Qt portal test\n"
                          "Exec=/bin/true\n");
    QFile entry(QDir(applicationsDirectory)
                    .filePath(applicationId + QStringLiteral(".desktop")));
    return entry.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && entry.write(contents) == contents.size();
}

bool writeEnvironmentProbe(const QString &path)
{
    const QByteArray contents = QByteArrayLiteral(
        "if test \"${QT_NO_XDG_DESKTOP_PORTAL+x}\" = x; then\n"
        "  printf set > \"$1\"\n"
        "else\n"
        "  printf unset > \"$1\"\n"
        "fi\n"
        "exec /bin/sleep 5\n");
    QFile script(path);
    return script.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && script.write(contents) == contents.size();
}

class NotifyReceiver final {
public:
    NotifyReceiver()
        : address_(QByteArrayLiteral("@ghostty-qt-application-")
                   + QByteArray::number(QCoreApplication::applicationPid())
                   + '-' + QUuid::createUuid().toByteArray(QUuid::Id128))
        , descriptor_(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0))
    {
        if (descriptor_ < 0) {
            errorCode_ = errno;
            error_ = QString::fromLocal8Bit(std::strerror(errno));
            return;
        }
        struct sockaddr_un socketAddress{};
        socketAddress.sun_family = AF_UNIX;
        const auto pathLength = static_cast<std::size_t>(address_.size());
        std::memcpy(socketAddress.sun_path, address_.constData(), pathLength);
        socketAddress.sun_path[0] = '\0';
        const socklen_t length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + pathLength);
        if (::bind(descriptor_,
                   reinterpret_cast<const struct sockaddr *>(&socketAddress),
                   length)
            != 0) {
            errorCode_ = errno;
            error_ = QString::fromLocal8Bit(std::strerror(errno));
        }
    }

    ~NotifyReceiver()
    {
        if (descriptor_ >= 0) (void)::close(descriptor_);
    }

    Q_DISABLE_COPY_MOVE(NotifyReceiver)

    [[nodiscard]] bool isValid() const
    {
        return descriptor_ >= 0 && error_.isEmpty();
    }
    [[nodiscard]] int errorCode() const { return errorCode_; }
    [[nodiscard]] const QString &errorString() const { return error_; }
    [[nodiscard]] const QByteArray &address() const { return address_; }

    QByteArray receive(int timeoutMilliseconds)
    {
        struct pollfd descriptor{
            .fd = descriptor_,
            .events = POLLIN,
            .revents = 0,
        };
        int ready = -1;
        do {
            ready = ::poll(&descriptor, 1, timeoutMilliseconds);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || (descriptor.revents & POLLIN) == 0) return {};

        std::array<char, 512> buffer{};
        ssize_t size = -1;
        do {
            size = ::recv(descriptor_, buffer.data(), buffer.size(), 0);
        } while (size < 0 && errno == EINTR);
        if (size <= 0) return {};
        return QByteArray(buffer.data(), static_cast<qsizetype>(size));
    }

private:
    QByteArray address_;
    int descriptor_ = -1;
    int errorCode_ = 0;
    QString error_;
};

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

std::optional<QProcessEnvironment>
portalApplicationEnvironment(const PrivateSessionBus &bus,
                             const QString &configHome)
{
    const QString runtimeDirectory = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY");
    if (runtimeDirectory.isEmpty() || display.isEmpty()) return std::nullopt;

    const QString socketPath = QFileInfo(display).isAbsolute()
        ? display
        : QDir(runtimeDirectory).filePath(display);
    const QFileInfo socket(socketPath);
    if (!socket.exists() || !socket.isOther()) return std::nullopt;

    QProcessEnvironment environment = applicationEnvironment(bus, configHome);
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("wayland"));
    environment.remove(QStringLiteral("QT_NO_XDG_DESKTOP_PORTAL"));
    environment.remove(QStringLiteral("SNAP"));
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

class PortalOrderingEndpoint final : public QDBusVirtualObject {
public:
    explicit PortalOrderingEndpoint(const QDBusConnection &connection)
        : connection_(connection)
    {}

    QString introspect(const QString &path) const override
    {
        Q_UNUSED(path);
        return {};
    }

    bool handleMessage(const QDBusMessage &message,
                       const QDBusConnection &connection) override
    {
        Q_UNUSED(connection);
        const QString sender = message.service();
        if (message.interface()
                == QLatin1StringView("org.freedesktop.host.portal.Registry")
            && message.member() == QLatin1StringView("Register")) {
            ++registerCount;
            calls.append(QStringLiteral("Register"));
            registerSender = sender;
            registerSignature = message.signature();

            if (associatedApplicationIds_.contains(sender)) {
                ++duplicateAssociationCount;
                connection_.send(message.createErrorReply(
                    QStringLiteral("org.freedesktop.portal.Error.Failed"),
                    QStringLiteral(
                        "Could not register app ID: Connection already associated with an application ID")));
                return true;
            }

            const QString requestedApplicationId =
                message.arguments().value(0).toString();
            registeredApplicationId = requestedApplicationId;
            if (rejectRegistrationAsMissing) {
                connection_.send(message.createErrorReply(
                    QStringLiteral("org.freedesktop.portal.Error.Failed"),
                    QStringLiteral(
                        "Could not register app ID: App info not found for '%1'")
                        .arg(requestedApplicationId)));
                return true;
            }
            associatedApplicationIds_.insert(sender, requestedApplicationId);
            connection_.send(message.createReply());
            return true;
        }

        const bool applicationPortalMethod =
            message.interface().startsWith(
                QLatin1StringView("org.freedesktop.portal."))
            && message.interface()
                != QLatin1StringView("org.freedesktop.DBus.Properties")
            && message.interface()
                != QLatin1StringView("org.freedesktop.DBus.Introspectable");
        if (applicationPortalMethod
            && !associatedApplicationIds_.contains(sender)) {
            ++implicitAssociationCount;
            implicitAssociationCall =
                message.interface() + u'.' + message.member();
            associatedApplicationIds_.insert(sender,
                                             QStringLiteral("<implicit>"));
        }

        if (message.interface()
                == QLatin1StringView("org.freedesktop.portal.GlobalShortcuts")
            && message.member() == QLatin1StringView("CreateSession")) {
            ++createSessionCount;
            calls.append(QStringLiteral("CreateSession"));
            createSessionSender = sender;
            createSessionSignature = message.signature();

            connection_.send(message.createReply(
                QVariant::fromValue(QDBusObjectPath(QStringLiteral(
                    "/org/freedesktop/portal/desktop/request/ghostty_qt_test/create")))));
            return true;
        }
        return false;
    }

    int registerCount = 0;
    int createSessionCount = 0;
    int implicitAssociationCount = 0;
    int duplicateAssociationCount = 0;
    bool rejectRegistrationAsMissing = false;
    QString registerSender;
    QString createSessionSender;
    QString registeredApplicationId;
    QString registerSignature;
    QString createSessionSignature;
    QString implicitAssociationCall;
    QStringList calls;

private:
    QDBusConnection connection_;
    QHash<QString, QString> associatedApplicationIds_;
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
    void hostPortalRegistrationPrecedesGlobalShortcutSession_data();
    void hostPortalRegistrationPrecedesGlobalShortcutSession();
    void systemdReadinessAndReloadBelongToPrimary();
#if GHOSTTY_QT_TEST_CONFIG_ENABLED
    void residentPrimaryIsReactivatedByBareSecondLaunch();
    void falseLauncherLeavesPrimaryAtZeroUntilTrueLauncherActivates();
#endif
};

void ApplicationSingleInstanceTest::systemdReadinessAndReloadBelongToPrimary()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    NotifyReceiver notify;
    if (!notify.isValid()
        && (notify.errorCode() == EPERM || notify.errorCode() == EACCES)) {
        QSKIP("The managed environment forbids AF_UNIX datagram sockets");
    }
    QVERIFY2(notify.isValid(), qPrintable(notify.errorString()));

    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir configHome(QDir::current().filePath(
        QStringLiteral("tmp/application-systemd-notify-XXXXXX")));
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
    environment.insert(QStringLiteral("NOTIFY_SOCKET"),
                       QString::fromLatin1(notify.address()));
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
    QCOMPARE(notify.receive(3000), QByteArrayLiteral("READY=1"));

    QVERIFY(::kill(static_cast<pid_t>(primary.processId()), SIGUSR2) == 0);
    const QByteArray reloading = notify.receive(3000);
    QVERIFY2(
        reloading.startsWith(QByteArrayLiteral("RELOADING=1\nMONOTONIC_USEC=")),
        reloading.constData());
    QCOMPARE(notify.receive(10'000), QByteArrayLiteral("READY=1"));

    QProcess secondary;
    secondary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    secondary.setArguments({QStringLiteral("--single-instance=true")});
    secondary.setProcessEnvironment(environment);
    secondary.start();
    QVERIFY(secondary.waitForStarted(3000));
    QVERIFY2(secondary.waitForFinished(10'000),
             qPrintable(
                 processFailure(secondary, secondary.readAllStandardOutput())));
    QCOMPARE(secondary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(secondary.exitCode(), 0);

    QVERIFY2(
        waitForMarker(primary, output,
                      QByteArrayView("GHOSTTY_QT_DESKTOP_ACTIVATION_CREATED"),
                      10'000),
        qPrintable(processFailure(primary, output)));
    QVERIFY2(primary.waitForFinished(10'000),
             qPrintable(processFailure(primary, output)));
    QCOMPARE(primary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(primary.exitCode(), 0);
    QCOMPARE(notify.receive(100), QByteArray());
}

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
    QTest::newRow("new-tab") << QStringList{
        QStringLiteral("+new-tab"),
        QStringLiteral("--surface-id=0"),
        QStringLiteral("--shell-integration=none"),
        QStringLiteral("--title=remote tab"),
        QStringLiteral("-e"),
        QStringLiteral("/bin/sleep"),
        QStringLiteral("1"),
    };
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
    QTest::newRow("new-tab") << QStringList{
        QStringLiteral("+new-tab"),
        QStringLiteral("--surface-id=0"),
        QStringLiteral("--title=cold tab"),
        QStringLiteral("-e"),
        QStringLiteral("/bin/true"),
    };
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

void ApplicationSingleInstanceTest::
    hostPortalRegistrationPrecedesGlobalShortcutSession_data()
{
    QTest::addColumn<bool>("installDesktopEntry");
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
    QTest::newRow("discoverable-metadata") << true;
#endif
    QTest::newRow("missing-metadata") << false;
}

void ApplicationSingleInstanceTest::
    hostPortalRegistrationPrecedesGlobalShortcutSession()
{
    QFETCH(bool, installDesktopEntry);
#if !GHOSTTY_QT_TEST_CONFIG_ENABLED
    Q_UNUSED(installDesktopEntry);
    QSKIP("the real Ghostty config helper is unavailable");
#else
    const QString customApplicationId =
        QStringLiteral("org.example.GhosttyQtPortalOrdering");
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
    QVERIFY(QDir().mkpath(QDir::current().filePath(QStringLiteral("tmp"))));
    QTemporaryDir configHome(QDir::current().filePath(
        QStringLiteral("tmp/application-portal-ordering-XXXXXX")));
    QVERIFY(configHome.isValid());
    QVERIFY(writeGhosttyConfig(
        configHome.path(),
        QByteArrayLiteral("initial-window = false\n"
                          "quit-after-last-window-closed = false\n"
                          "confirm-close-surface = false\n"
                          "class = org.example.GhosttyQtPortalOrdering\n"
                          "keybind = global:ctrl+g=new_tab\n")));
    QVERIFY(writeFrontendConfig(
        configHome.path(), QByteArrayLiteral("single-instance = false\n")));

    const QDir root(configHome.path());
    const QString dataHome = root.filePath(QStringLiteral("data-home"));
    const QString dataDirectory = root.filePath(QStringLiteral("data-dirs"));
    const QString probeScript =
        root.filePath(QStringLiteral("environment-probe.sh"));
    const QString probeOutput =
        root.filePath(QStringLiteral("environment-probe.txt"));
    QVERIFY(QDir().mkpath(dataHome));
    QVERIFY(QDir().mkpath(dataDirectory));
    QVERIFY(writeEnvironmentProbe(probeScript));
    if (installDesktopEntry) {
        QVERIFY(writeDesktopEntry(dataHome, customApplicationId));
    }

    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));
    auto environment = portalApplicationEnvironment(bus, configHome.path());
    if (!environment.has_value()) {
        QSKIP(
            "a host Wayland compositor is required to instantiate Qt's Unix portal services");
    }
    environment->insert(QStringLiteral("XDG_DATA_HOME"), dataHome);
    environment->insert(QStringLiteral("XDG_DATA_DIRS"), dataDirectory);

    PortalOrderingEndpoint portal(bus.server());
    portal.rejectRegistrationAsMissing = !installDesktopEntry;
    QVERIFY(bus.server().registerService(
        QStringLiteral("org.freedesktop.portal.Desktop")));
    QVERIFY(bus.server().registerVirtualObject(
        QStringLiteral("/org/freedesktop/portal/desktop"), &portal,
        QDBusConnection::SubPath));

    QProcess primary;
    primary.setProgram(QStringLiteral(GHOSTTY_QT_TEST_EXECUTABLE));
    primary.setArguments({QStringLiteral("--single-instance=true"),
                          QStringLiteral("--initial-window=true"),
                          QStringLiteral("-e"), QStringLiteral("/bin/sh"),
                          probeScript, probeOutput});
    primary.setProcessEnvironment(*environment);
    primary.start();
    QVERIFY(primary.waitForStarted(3000));
    const auto cleanup = qScopeGuard([&primary] {
        if (primary.state() == QProcess::NotRunning) return;
        primary.kill();
        primary.waitForFinished(3000);
    });

    QTRY_VERIFY_WITH_TIMEOUT(portal.createSessionCount > 0
                                 || primary.state() == QProcess::NotRunning,
                             15'000);
    QByteArray output = primary.readAllStandardOutput();
    QVERIFY2(portal.createSessionCount > 0,
             qPrintable(processFailure(primary, output)));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(probeOutput)
                                 || primary.state() == QProcess::NotRunning,
                             10'000);
    QFile probe(probeOutput);
    QVERIFY2(probe.open(QIODevice::ReadOnly),
             qPrintable(processFailure(primary, output)));
    QCOMPARE(probe.readAll(), QByteArrayLiteral("unset"));

    // Leave one event-loop turn for a wrongly queued late registration. The
    // fake models the host registry's per-connection association rule, so the
    // old ordering records both an implicit and a duplicate association.
    QTest::qWait(100);
    const QByteArray errorOutput = primary.readAllStandardError();
    QCOMPARE(primary.state(), QProcess::Running);
    QCOMPARE(portal.createSessionCount, 1);
    QCOMPARE(portal.duplicateAssociationCount, 0);
    QCOMPARE(portal.createSessionSignature, QStringLiteral("a{sv}"));
    QVERIFY(!portal.createSessionSender.isEmpty());
    if (installDesktopEntry) {
        if (portal.registerCount == 0
            && portal.implicitAssociationCount == 1
            && portal.calls
                == QStringList({QStringLiteral("CreateSession")})) {
            QSKIP("This Qt build does not provide host portal registry registration");
        }
        QCOMPARE(portal.registerCount, 1);
        QVERIFY2(portal.implicitAssociationCount == 0,
                 qPrintable(QStringLiteral("implicit association via %1")
                                .arg(portal.implicitAssociationCall)));
        QCOMPARE(portal.calls,
                 QStringList({QStringLiteral("Register"),
                              QStringLiteral("CreateSession")}));
        QVERIFY(!portal.registerSender.isEmpty());
        QCOMPARE(portal.createSessionSender, portal.registerSender);
        QCOMPARE(portal.registeredApplicationId, customApplicationId);
        QCOMPARE(portal.registerSignature, QStringLiteral("sa{sv}"));
    } else {
        QCOMPARE(portal.registerCount, 0);
        QCOMPARE(portal.implicitAssociationCount, 1);
        QVERIFY2(portal.implicitAssociationCall.startsWith(
                     QStringLiteral("org.freedesktop.portal.")),
                 qPrintable(portal.implicitAssociationCall));
        QCOMPARE(portal.calls, QStringList({QStringLiteral("CreateSession")}));
        QVERIFY(portal.registerSender.isEmpty());
        QVERIFY(portal.registeredApplicationId.isEmpty());
        QVERIFY(portal.registerSignature.isEmpty());
    }
    QVERIFY2(!errorOutput.contains("Failed to register with host portal"),
             errorOutput.constData());
    QVERIFY2(!errorOutput.contains("Connection already associated"),
             errorOutput.constData());
    QVERIFY2(!errorOutput.contains("App info not found"),
             errorOutput.constData());
#endif
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
