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

// Parse one strict UTF-8 frontend configuration document. The grammar is one
// scalar `key = value` assignment per non-empty line. Comments must occupy a
// complete line, and duplicate or unknown keys are errors.
[[nodiscard]] std::expected<FrontendConfigValues, QString>
parseFrontendConfig(QByteArrayView contents, QStringView sourceName = {});

// A missing file is a successful load of built-in defaults. Every other I/O
// or syntax failure is returned without a partial configuration.
[[nodiscard]] FrontendConfigLoadResult
loadFrontendConfigFile(const QString &path);

Q_DECLARE_METATYPE(TabsLocation)
Q_DECLARE_METATYPE(FrontendConfigValues)
Q_DECLARE_METATYPE(FrontendConfigSnapshot)
