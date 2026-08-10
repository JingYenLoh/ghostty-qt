#pragma once

#include "ghostty_config_values.h"
#include "quick_terminal.h"

#include <QByteArrayView>
#include <QMetaType>
#include <QString>
#include <QStringView>

#include <expected>
#include <functional>

enum class TabsLocation {
    Top,
    Bottom,
};

struct FrontendConfigValues {
    SingleInstanceMode singleInstanceMode = SingleInstanceMode::Detect;
    TabsLocation tabsLocation = TabsLocation::Top;
    bool wideTabs = true;
    bool horizontalTabScroll = true;
    QuickTerminalLayerShellOptions quickTerminalLayerShell;

    bool operator==(const FrontendConfigValues &) const = default;
};

struct FrontendConfigSnapshot {
    FrontendConfigValues values;
    // Empty when no file exists and built-in defaults are in effect.
    QString sourcePath;

    bool operator==(const FrontendConfigSnapshot &) const = default;
};

using FrontendConfigLoadResult = std::expected<FrontendConfigSnapshot, QString>;
using FrontendConfigLoader =
    std::function<FrontendConfigLoadResult(const QString &path)>;

// This file controls only a handful of scalar Qt settings. A fixed ceiling
// keeps startup and asynchronous reload work proportional even when the path
// is replaced by an unexpectedly large file.
inline constexpr qsizetype MaximumFrontendConfigFileSize = 1024 * 1024;

// Parse the Qt-owned subset of one mixed ghostty-qt configuration document.
// Recognized frontend keys use one strict UTF-8 scalar `key = value`
// assignment per non-empty line. Every other line is left for the pinned
// Ghostty parser, which owns its grammar, validation, and duplicate semantics.
[[nodiscard]] std::expected<FrontendConfigValues, QString>
parseFrontendConfig(QByteArrayView contents, QStringView sourceName = {});

// A missing file is a successful load of built-in defaults. Every other I/O,
// file-type, size, or syntax failure is returned without a partial
// configuration.
[[nodiscard]] FrontendConfigLoadResult
loadFrontendConfigFile(const QString &path);

Q_DECLARE_METATYPE(TabsLocation)
Q_DECLARE_METATYPE(FrontendConfigValues)
Q_DECLARE_METATYPE(FrontendConfigSnapshot)
