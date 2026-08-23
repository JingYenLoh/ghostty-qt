#pragma once

#include <QByteArray>
#include <QVector>
#include <QtGlobal>

#include <utility>

enum class TerminalCommandKind : quint8 {
    Shell,
    Direct,
};

// Ghostty command strings are byte-valued and have two intentionally distinct
// execution modes. Shell commands are passed unchanged to `/bin/sh -c`;
// direct commands preserve every argv entry, including empty arguments. The
// default-shell bit retains finalization provenance so process-activity
// tracking can distinguish Ghostty's resolved login shell from a configured
// command that happens to use the shell form.
struct TerminalCommand {
    TerminalCommandKind kind = TerminalCommandKind::Shell;
    QByteArray shellCommand;
    QVector<QByteArray> directArguments;
    bool defaultShell = false;

    [[nodiscard]] static TerminalCommand shell(QByteArray command,
                                               bool isDefaultShell = false)
    {
        return {
            .kind = TerminalCommandKind::Shell,
            .shellCommand = std::move(command),
            .directArguments = {},
            .defaultShell = isDefaultShell,
        };
    }

    [[nodiscard]] static TerminalCommand direct(QVector<QByteArray> arguments)
    {
        return {
            .kind = TerminalCommandKind::Direct,
            .shellCommand = {},
            .directArguments = std::move(arguments),
            .defaultShell = false,
        };
    }

    bool operator==(const TerminalCommand &) const = default;
};
