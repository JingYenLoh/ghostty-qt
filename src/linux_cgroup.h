#pragma once

#include "linux_cgroup_config.h"

#include <QString>

#include <chrono>
#include <expected>

class QDBusConnection;

struct LinuxCgroupWaitOptions {
    std::chrono::milliseconds timeout = std::chrono::milliseconds(250);
    std::chrono::milliseconds pollInterval = std::chrono::milliseconds(25);
    QString procRoot = QStringLiteral("/proc");
};

[[nodiscard]] bool linuxCgroupEnabled(LinuxCgroupMode mode,
                                      bool singleInstance) noexcept;
[[nodiscard]] QString linuxCgroupScopeName(quint32 pid);
[[nodiscard]] std::expected<QString, QString>
currentLinuxCgroupPath(quint32 pid,
                       const QString &procRoot = QStringLiteral("/proc"));
[[nodiscard]] std::expected<void, QString>
moveProcessToLinuxCgroup(quint32 pid, const LinuxCgroupConfig &config,
                         bool singleInstance);
[[nodiscard]] std::expected<void, QString>
moveProcessToLinuxCgroup(quint32 pid, const LinuxCgroupConfig &config,
                         bool singleInstance, const QDBusConnection &connection,
                         LinuxCgroupWaitOptions waitOptions = {});
