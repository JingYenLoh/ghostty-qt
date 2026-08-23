#include "desktop/linux_cgroup.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QList>
#include <QMetaType>
#include <QVariant>

#include <algorithm>
#include <thread>
#include <utility>

namespace LinuxCgroupDbus {

struct Property {
    QString name;
    QDBusVariant value;
};

using PropertyList = QList<Property>;

struct AuxiliaryUnit {
    QString name;
    PropertyList properties;
};

using AuxiliaryUnitList = QList<AuxiliaryUnit>;
using PidList = QList<quint32>;

QDBusArgument &operator<<(QDBusArgument &argument, const Property &property)
{
    argument.beginStructure();
    argument << property.name << property.value;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                Property &property)
{
    argument.beginStructure();
    argument >> property.name >> property.value;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const AuxiliaryUnit &unit)
{
    argument.beginStructure();
    argument << unit.name << unit.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                AuxiliaryUnit &unit)
{
    argument.beginStructure();
    argument >> unit.name >> unit.properties;
    argument.endStructure();
    return argument;
}

} // namespace LinuxCgroupDbus

Q_DECLARE_METATYPE(LinuxCgroupDbus::Property)
Q_DECLARE_METATYPE(LinuxCgroupDbus::PropertyList)
Q_DECLARE_METATYPE(LinuxCgroupDbus::AuxiliaryUnit)
Q_DECLARE_METATYPE(LinuxCgroupDbus::AuxiliaryUnitList)
Q_DECLARE_METATYPE(LinuxCgroupDbus::PidList)

namespace {

constexpr auto SystemdService = "org.freedesktop.systemd1";
constexpr auto SystemdPath = "/org/freedesktop/systemd1";
constexpr auto SystemdManagerInterface = "org.freedesktop.systemd1.Manager";
constexpr auto StartTransientUnit = "StartTransientUnit";

void registerSystemdTypes()
{
    static const bool registered = [] {
        qDBusRegisterMetaType<LinuxCgroupDbus::Property>();
        qDBusRegisterMetaType<LinuxCgroupDbus::PropertyList>();
        qDBusRegisterMetaType<LinuxCgroupDbus::AuxiliaryUnit>();
        qDBusRegisterMetaType<LinuxCgroupDbus::AuxiliaryUnitList>();
        qDBusRegisterMetaType<LinuxCgroupDbus::PidList>();
        return true;
    }();
    Q_UNUSED(registered);
}

LinuxCgroupDbus::Property property(QString name, QVariant value)
{
    return {
        .name = std::move(name),
        .value = QDBusVariant(std::move(value)),
    };
}

std::expected<void, QString> createScope(quint32 pid,
                                         const LinuxCgroupConfig &config,
                                         const QDBusConnection &connection)
{
    if (!connection.isConnected()) {
        return std::unexpected(
            QStringLiteral("The session D-Bus is unavailable"));
    }

    registerSystemdTypes();

    LinuxCgroupDbus::PropertyList properties;
    if (config.memoryLimitBytes.has_value()) {
        properties.append(
            property(QStringLiteral("MemoryHigh"),
                     QVariant::fromValue(*config.memoryLimitBytes)));
    }
    if (config.processesLimit.has_value()) {
        properties.append(
            property(QStringLiteral("TasksMax"),
                     QVariant::fromValue(*config.processesLimit)));
    }
    properties.append(property(QStringLiteral("ManagedOOMMemoryPressure"),
                               QVariant::fromValue(QStringLiteral("kill"))));
    properties.append(
        property(QStringLiteral("PIDs"),
                 QVariant::fromValue(LinuxCgroupDbus::PidList{pid})));

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(SystemdService), QString::fromLatin1(SystemdPath),
        QString::fromLatin1(SystemdManagerInterface),
        QString::fromLatin1(StartTransientUnit));
    call << linuxCgroupScopeName(pid) << QStringLiteral("fail")
         << QVariant::fromValue(properties)
         << QVariant::fromValue(LinuxCgroupDbus::AuxiliaryUnitList{});

    const QDBusMessage reply = connection.call(call, QDBus::Block);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        const QDBusError error(reply);
        return std::unexpected(
            QStringLiteral("Could not create transient systemd scope %1: %2")
                .arg(linuxCgroupScopeName(pid), error.message()));
    }
    const QVariantList arguments = reply.arguments();
    if (arguments.size() != 1
        || arguments.front().metaType()
            != QMetaType::fromType<QDBusObjectPath>()) {
        return std::unexpected(
            QStringLiteral(
                "systemd returned an invalid StartTransientUnit reply for %1")
                .arg(linuxCgroupScopeName(pid)));
    }
    return {};
}

bool pathEndsInScope(QStringView path, QStringView scope)
{
    const qsizetype separator = path.lastIndexOf(u'/');
    return separator >= 0 && path.sliced(separator + 1) == scope;
}

} // namespace

bool linuxCgroupEnabled(LinuxCgroupMode mode, bool singleInstance) noexcept
{
    switch (mode) {
    case LinuxCgroupMode::Never: return false;
    case LinuxCgroupMode::Always: return true;
    case LinuxCgroupMode::SingleInstance: return singleInstance;
    }
    return false;
}

QString linuxCgroupScopeName(quint32 pid)
{
    return QStringLiteral("app-ghostty-surface-transient-%1.scope").arg(pid);
}

namespace {

QDBusConnection defaultLinuxCgroupConnection()
{
    if (!qEnvironmentVariableIsEmpty("DBUS_STARTER_ADDRESS")) {
        return QDBusConnection::connectToBus(
            QDBusConnection::ActivationBus,
            QStringLiteral("ghostty_qt_activation_bus"));
    }
    return QDBusConnection::sessionBus();
}

} // namespace

std::expected<void, QString>
moveProcessToLinuxCgroup(quint32 pid, const LinuxCgroupConfig &config,
                         bool singleInstance)
{
    return moveProcessToLinuxCgroup(pid, config, singleInstance,
                                    defaultLinuxCgroupConnection());
}

std::expected<QString, QString> currentLinuxCgroupPath(quint32 pid,
                                                       const QString &procRoot)
{
    const QString path =
        QDir(procRoot).filePath(QStringLiteral("%1/cgroup").arg(pid));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("Could not read %1: %2")
                                   .arg(path, file.errorString()));
    }

    constexpr qint64 MaximumLineLength = 4096;
    QByteArray line = file.readLine(MaximumLineLength + 1);
    if (line.isEmpty()) {
        return std::unexpected(
            QStringLiteral("%1 did not contain a cgroup entry").arg(path));
    }
    if (line.size() > MaximumLineLength && !line.endsWith('\n')) {
        return std::unexpected(
            QStringLiteral("The cgroup entry in %1 is too long").arg(path));
    }

    const qsizetype separator = line.lastIndexOf(':');
    if (separator < 0) {
        return std::unexpected(
            QStringLiteral("The cgroup entry in %1 is malformed").arg(path));
    }
    const QByteArray cgroupPath = line.sliced(separator + 1).trimmed();
    if (cgroupPath.isEmpty()) {
        return std::unexpected(
            QStringLiteral("The cgroup entry in %1 has an empty path")
                .arg(path));
    }
    return QString::fromUtf8(cgroupPath);
}

std::expected<void, QString>
moveProcessToLinuxCgroup(quint32 pid, const LinuxCgroupConfig &config,
                         bool singleInstance, const QDBusConnection &connection,
                         LinuxCgroupWaitOptions waitOptions)
{
    if (!linuxCgroupEnabled(config.mode, singleInstance)) return {};
    if (pid == 0) {
        return std::unexpected(
            QStringLiteral("Cannot create a cgroup scope for PID 0"));
    }

    const std::expected<void, QString> created =
        createScope(pid, config, connection);
    if (!created.has_value()) return created;

    const QString expectedScope = linuxCgroupScopeName(pid);
    const std::chrono::milliseconds timeout =
        std::max(waitOptions.timeout, std::chrono::milliseconds::zero());
    const std::chrono::milliseconds pollInterval =
        std::max(waitOptions.pollInterval, std::chrono::milliseconds(1));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    QString lastDiagnostic;

    while (true) {
        const std::expected<QString, QString> current =
            currentLinuxCgroupPath(pid, waitOptions.procRoot);
        if (current.has_value()) {
            if (pathEndsInScope(*current, expectedScope)) return {};
            lastDiagnostic =
                QStringLiteral("current cgroup is %1").arg(*current);
        } else {
            lastDiagnostic = current.error();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        std::this_thread::sleep_for(
            std::min(pollInterval,
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - now)));
    }

    return std::unexpected(
        QStringLiteral("Timed out waiting for PID %1 to enter %2 (%3)")
            .arg(pid)
            .arg(expectedScope, lastDiagnostic));
}
