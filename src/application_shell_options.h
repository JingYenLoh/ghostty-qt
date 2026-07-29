#pragma once

#include "quick_terminal.h"

#include <QString>
#include <QVector>

struct CommandPaletteEntry {
    QString title;
    QString description;
    QString actionKey;
    QString action;

    bool operator==(const CommandPaletteEntry &) const = default;
};

struct AppNotificationOptions {
    bool clipboardCopy = true;
    bool configReload = true;

    bool operator==(const AppNotificationOptions &) const = default;
};

struct ApplicationShellOptions {
    QuickTerminalOptions quickTerminal;
    QVector<CommandPaletteEntry> commandPalette;
    AppNotificationOptions notifications;

    bool operator==(const ApplicationShellOptions &) const = default;
};
