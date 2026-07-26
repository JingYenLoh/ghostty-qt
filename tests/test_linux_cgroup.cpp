#include "linux_cgroup.h"
#include "private_session_bus.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#include <chrono>
#include <expected>
#include <future>
#include <limits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr auto SystemdService = "org.freedesktop.systemd1";
constexpr auto SystemdPath = "/org/freedesktop/systemd1";
constexpr auto SystemdManagerInterface = "org.freedesktop.systemd1.Manager";

struct CapturedProperty {
    QString name;
    QVariant value;
};

class FakeSystemdManager final : public QDBusVirtualObject {
public:
    enum class Reply {
        Success,
        Failure,
    };

    explicit FakeSystemdManager(Reply reply = Reply::Success)
        : reply_(reply)
    {}

    QString introspect(const QString &) const override
    {
        return QStringLiteral(
            "<interface name=\"org.freedesktop.systemd1.Manager\">"
            "<method name=\"StartTransientUnit\">"
            "<arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"a(sv)\" direction=\"in\"/>"
            "<arg type=\"a(sa(sv))\" direction=\"in\"/>"
            "<arg type=\"o\" direction=\"out\"/>"
            "</method>"
            "</interface>");
    }

    bool handleMessage(const QDBusMessage &message,
                       const QDBusConnection &connection) override
    {
        if (message.interface() != QString::fromLatin1(SystemdManagerInterface)
            || message.member() != QStringLiteral("StartTransientUnit")) {
            return false;
        }

        ++calls;
        lastMessage = message;
        if (reply_ == Reply::Failure) {
            connection.send(message.createErrorReply(
                QStringLiteral("org.freedesktop.systemd1.UnitExists"),
                QStringLiteral("scope collision")));
        } else {
            connection.send(
                message.createReply(QVariant::fromValue(QDBusObjectPath(
                    QStringLiteral("/org/freedesktop/systemd1/job/1")))));
        }
        return true;
    }

    int calls = 0;
    QDBusMessage lastMessage;

private:
    Reply reply_;
};

bool registerFakeSystemd(PrivateSessionBus &bus, FakeSystemdManager &manager)
{
    return bus.server().registerVirtualObject(QString::fromLatin1(SystemdPath),
                                              &manager)
        && bus.server().registerService(QString::fromLatin1(SystemdService));
}

bool writeCgroupEntry(const QString &root, quint32 pid,
                      const QByteArray &contents)
{
    const QString directory = QDir(root).filePath(QString::number(pid));
    if (!QDir().mkpath(directory)) return false;
    QFile file(QDir(directory).filePath(QStringLiteral("cgroup")));
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size();
}

std::vector<CapturedProperty> decodeProperties(const QVariant &serialized)
{
    std::vector<CapturedProperty> result;
    if (serialized.metaType() != QMetaType::fromType<QDBusArgument>()) {
        return result;
    }

    const QDBusArgument argument = serialized.value<QDBusArgument>();
    argument.beginArray();
    while (!argument.atEnd()) {
        QString name;
        QDBusVariant value;
        argument.beginStructure();
        argument >> name >> value;
        argument.endStructure();
        result.push_back({
            .name = std::move(name),
            .value = value.variant(),
        });
    }
    argument.endArray();
    return result;
}

QList<quint32> decodePids(const QVariant &serialized)
{
    if (serialized.metaType() != QMetaType::fromType<QDBusArgument>()) {
        return {};
    }

    QList<quint32> result;
    const QDBusArgument argument = serialized.value<QDBusArgument>();
    argument.beginArray();
    while (!argument.atEnd()) {
        quint32 pid = 0;
        argument >> pid;
        result.append(pid);
    }
    argument.endArray();
    return result;
}

void requirePrivateBus()
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"))
            .isEmpty()) {
        QSKIP("dbus-daemon is unavailable");
    }
}

} // namespace

class LinuxCgroupTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void modeGatingAndScopeNameAreExact();
    void readsCurrentUnifiedCgroupPath();
    void rejectsInvalidCurrentCgroupEntries();
    void sendsExactStartTransientUnitWire();
    void omitsUnsetResourceLimits();
    void reportsStartTransientUnitFailure();
    void reportsMoveVerificationTimeout();
};

void LinuxCgroupTest::modeGatingAndScopeNameAreExact()
{
    QVERIFY(!linuxCgroupEnabled(LinuxCgroupMode::Never, false));
    QVERIFY(!linuxCgroupEnabled(LinuxCgroupMode::Never, true));
    QVERIFY(linuxCgroupEnabled(LinuxCgroupMode::Always, false));
    QVERIFY(linuxCgroupEnabled(LinuxCgroupMode::Always, true));
    QVERIFY(!linuxCgroupEnabled(LinuxCgroupMode::SingleInstance, false));
    QVERIFY(linuxCgroupEnabled(LinuxCgroupMode::SingleInstance, true));

    QCOMPARE(linuxCgroupScopeName(1),
             QStringLiteral("app-ghostty-surface-transient-1.scope"));
    QCOMPARE(linuxCgroupScopeName(std::numeric_limits<quint32>::max()),
             QStringLiteral("app-ghostty-surface-transient-4294967295.scope"));
}

void LinuxCgroupTest::readsCurrentUnifiedCgroupPath()
{
    QTemporaryDir procRoot;
    QVERIFY(procRoot.isValid());
    constexpr quint32 Pid = 4182;

    QVERIFY(writeCgroupEntry(
        procRoot.path(), Pid,
        QByteArrayLiteral("0::/user.slice/user-1000.slice/app.scope\n"
                          "1:name=systemd:/ignored.scope\n")));
    const std::expected<QString, QString> unified =
        currentLinuxCgroupPath(Pid, procRoot.path());
    QVERIFY2(unified.has_value(), qPrintable(unified.error_or(QString{})));
    QCOMPARE(*unified, QStringLiteral("/user.slice/user-1000.slice/app.scope"));

    QVERIFY(writeCgroupEntry(
        procRoot.path(), Pid,
        QByteArrayLiteral("7:cpu,cpuacct:/legacy/session.scope\r\n")));
    const std::expected<QString, QString> legacy =
        currentLinuxCgroupPath(Pid, procRoot.path());
    QVERIFY2(legacy.has_value(), qPrintable(legacy.error_or(QString{})));
    QCOMPARE(*legacy, QStringLiteral("/legacy/session.scope"));
}

void LinuxCgroupTest::rejectsInvalidCurrentCgroupEntries()
{
    QTemporaryDir procRoot;
    QVERIFY(procRoot.isValid());
    constexpr quint32 Pid = 72;

    QVERIFY(writeCgroupEntry(procRoot.path(), Pid,
                             QByteArrayLiteral("not-a-cgroup\n")));
    const auto malformed = currentLinuxCgroupPath(Pid, procRoot.path());
    QVERIFY(!malformed.has_value());
    QVERIFY(malformed.error().contains(QStringLiteral("malformed")));

    QVERIFY(writeCgroupEntry(procRoot.path(), Pid, QByteArrayLiteral("0::\n")));
    const auto empty = currentLinuxCgroupPath(Pid, procRoot.path());
    QVERIFY(!empty.has_value());
    QVERIFY(empty.error().contains(QStringLiteral("empty path")));

    const auto missing = currentLinuxCgroupPath(Pid + 1, procRoot.path());
    QVERIFY(!missing.has_value());
    QVERIFY(missing.error().contains(QStringLiteral("Could not read")));
}

void LinuxCgroupTest::sendsExactStartTransientUnitWire()
{
    requirePrivateBus();
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));
    FakeSystemdManager manager;
    QVERIFY(registerFakeSystemd(bus, manager));

    QTemporaryDir procRoot;
    QVERIFY(procRoot.isValid());
    constexpr quint32 Pid = 424242;
    QVERIFY(writeCgroupEntry(
        procRoot.path(), Pid,
        QByteArrayLiteral(
            "0::/user.slice/app-ghostty-surface-transient-424242.scope\n")));

    const LinuxCgroupConfig config{
        .mode = LinuxCgroupMode::Always,
        .memoryLimitBytes = std::numeric_limits<quint64>::max() - 1,
        .processesLimit = std::numeric_limits<quint64>::max(),
        .hardFail = true,
    };
    auto future = std::async(std::launch::async, [&bus, &procRoot, config] {
        return moveProcessToLinuxCgroup(Pid, config, false, bus.client(),
                                        {
                                            .timeout = 100ms,
                                            .pollInterval = 5ms,
                                            .procRoot = procRoot.path(),
                                        });
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms) == std::future_status::ready,
                             3000);
    const std::expected<void, QString> result = future.get();
    QVERIFY2(result.has_value(), qPrintable(result.error_or(QString{})));

    QCOMPARE(manager.calls, 1);
    // Successful delivery to the fake's well-known service already proves
    // the destination. Qt exposes the sender's unique name in service() on
    // the received copy.
    QCOMPARE(manager.lastMessage.path(), QString::fromLatin1(SystemdPath));
    QCOMPARE(manager.lastMessage.interface(),
             QString::fromLatin1(SystemdManagerInterface));
    QCOMPARE(manager.lastMessage.member(),
             QStringLiteral("StartTransientUnit"));
    QCOMPARE(manager.lastMessage.signature(),
             QStringLiteral("ssa(sv)a(sa(sv))"));

    const QVariantList arguments = manager.lastMessage.arguments();
    QCOMPARE(arguments.size(), 4);
    QCOMPARE(arguments.at(0).toString(), linuxCgroupScopeName(Pid));
    QCOMPARE(arguments.at(1).toString(), QStringLiteral("fail"));

    const std::vector<CapturedProperty> properties =
        decodeProperties(arguments.at(2));
    QCOMPARE(properties.size(), std::size_t(4));
    QCOMPARE(properties.at(0).name, QStringLiteral("MemoryHigh"));
    QCOMPARE(properties.at(0).value.metaType(), QMetaType::fromType<quint64>());
    QCOMPARE(properties.at(0).value.toULongLong(),
             std::numeric_limits<quint64>::max() - 1);
    QCOMPARE(properties.at(1).name, QStringLiteral("TasksMax"));
    QCOMPARE(properties.at(1).value.metaType(), QMetaType::fromType<quint64>());
    QCOMPARE(properties.at(1).value.toULongLong(),
             std::numeric_limits<quint64>::max());
    QCOMPARE(properties.at(2).name, QStringLiteral("ManagedOOMMemoryPressure"));
    QCOMPARE(properties.at(2).value.metaType(), QMetaType::fromType<QString>());
    QCOMPARE(properties.at(2).value.toString(), QStringLiteral("kill"));
    QCOMPARE(properties.at(3).name, QStringLiteral("PIDs"));
    QCOMPARE(decodePids(properties.at(3).value), QList<quint32>{Pid});

    QCOMPARE(arguments.at(3).metaType(), QMetaType::fromType<QDBusArgument>());
    const QDBusArgument auxiliary = arguments.at(3).value<QDBusArgument>();
    QCOMPARE(auxiliary.currentSignature(), QStringLiteral("a(sa(sv))"));
    auxiliary.beginArray();
    QVERIFY(auxiliary.atEnd());
    auxiliary.endArray();
}

void LinuxCgroupTest::omitsUnsetResourceLimits()
{
    requirePrivateBus();
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));
    FakeSystemdManager manager;
    QVERIFY(registerFakeSystemd(bus, manager));

    QTemporaryDir procRoot;
    QVERIFY(procRoot.isValid());
    constexpr quint32 Pid = 131;
    QVERIFY(writeCgroupEntry(
        procRoot.path(), Pid,
        QByteArrayLiteral("0::/app-ghostty-surface-transient-131.scope\n")));

    auto future = std::async(std::launch::async, [&bus, &procRoot] {
        return moveProcessToLinuxCgroup(Pid, {.mode = LinuxCgroupMode::Always},
                                        false, bus.client(),
                                        {
                                            .timeout = 100ms,
                                            .pollInterval = 5ms,
                                            .procRoot = procRoot.path(),
                                        });
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms) == std::future_status::ready,
                             3000);
    const auto result = future.get();
    QVERIFY2(result.has_value(), qPrintable(result.error_or(QString{})));

    const std::vector<CapturedProperty> properties =
        decodeProperties(manager.lastMessage.arguments().at(2));
    QCOMPARE(properties.size(), std::size_t(2));
    QCOMPARE(properties.at(0).name, QStringLiteral("ManagedOOMMemoryPressure"));
    QCOMPARE(properties.at(1).name, QStringLiteral("PIDs"));
}

void LinuxCgroupTest::reportsStartTransientUnitFailure()
{
    requirePrivateBus();
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));
    FakeSystemdManager manager(FakeSystemdManager::Reply::Failure);
    QVERIFY(registerFakeSystemd(bus, manager));

    QTemporaryDir procRoot;
    QVERIFY(procRoot.isValid());
    constexpr quint32 Pid = 909;
    auto future = std::async(std::launch::async, [&bus, &procRoot] {
        return moveProcessToLinuxCgroup(Pid, {.mode = LinuxCgroupMode::Always},
                                        false, bus.client(),
                                        {
                                            .timeout = 25ms,
                                            .pollInterval = 5ms,
                                            .procRoot = procRoot.path(),
                                        });
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms) == std::future_status::ready,
                             3000);
    const auto result = future.get();
    QVERIFY(!result.has_value());
    QVERIFY(result.error().contains(QStringLiteral("scope collision")));
    QCOMPARE(manager.calls, 1);
}

void LinuxCgroupTest::reportsMoveVerificationTimeout()
{
    requirePrivateBus();
    PrivateSessionBus bus;
    QVERIFY2(bus.start(), qPrintable(bus.errorString()));
    FakeSystemdManager manager;
    QVERIFY(registerFakeSystemd(bus, manager));

    QTemporaryDir procRoot;
    QVERIFY(procRoot.isValid());
    constexpr quint32 Pid = 808;
    QVERIFY(
        writeCgroupEntry(procRoot.path(), Pid,
                         QByteArrayLiteral("0::/user.slice/original.scope\n")));

    auto future = std::async(std::launch::async, [&bus, &procRoot] {
        return moveProcessToLinuxCgroup(Pid, {.mode = LinuxCgroupMode::Always},
                                        false, bus.client(),
                                        {
                                            .timeout = 35ms,
                                            .pollInterval = 10ms,
                                            .procRoot = procRoot.path(),
                                        });
    });
    QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(0ms) == std::future_status::ready,
                             3000);
    const auto result = future.get();
    QVERIFY(!result.has_value());
    QVERIFY(result.error().contains(QStringLiteral("Timed out waiting")));
    QVERIFY(result.error().contains(QStringLiteral("original.scope")));
    QCOMPARE(manager.calls, 1);
}

QTEST_GUILESS_MAIN(LinuxCgroupTest)

#include "test_linux_cgroup.moc"
