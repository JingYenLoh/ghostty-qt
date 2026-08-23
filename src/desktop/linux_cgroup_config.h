#pragma once

#include <QtGlobal>

#include <optional>

enum class LinuxCgroupMode {
    Never,
    Always,
    SingleInstance,
};

struct LinuxCgroupConfig {
    LinuxCgroupMode mode = LinuxCgroupMode::SingleInstance;
    std::optional<quint64> memoryLimitBytes;
    std::optional<quint64> processesLimit;
    bool hardFail = false;

    bool operator==(const LinuxCgroupConfig &) const = default;
};
