#pragma once

#include "terminal_session_options.h"

#include <QString>
#include <QtGlobal>

class QClipboard;

enum class TerminalClipboardSource : quint8 {
    Standard,
    Primary,
};

struct TerminalClipboardWriteTargets {
    bool standard = false;
    bool primary = false;
};

constexpr TerminalClipboardWriteTargets terminalClipboardWriteTargets(
    TerminalClipboardDestination destination, bool supportsPrimary)
{
    switch (destination) {
    case TerminalClipboardDestination::Standard:
        return {.standard = true};
    case TerminalClipboardDestination::Primary:
        return supportsPrimary
            ? TerminalClipboardWriteTargets{.primary = true}
            : TerminalClipboardWriteTargets{.standard = true};
    case TerminalClipboardDestination::PrimaryAndStandard:
        return {.standard = true, .primary = supportsPrimary};
    }
    return {};
}

constexpr TerminalClipboardSource terminalMiddleClickSource(
    TerminalCopyOnSelectMode copyOnSelect, bool supportsPrimary)
{
    if (copyOnSelect == TerminalCopyOnSelectMode::PrimaryAndClipboard
        || !supportsPrimary) {
        return TerminalClipboardSource::Standard;
    }
    return TerminalClipboardSource::Primary;
}

// These are GUI adapters: callers must remain on the QGuiApplication thread.
void writeTerminalClipboard(QClipboard *clipboard, const QString &text,
                            TerminalClipboardDestination destination);
QString readMiddleClickClipboard(QClipboard *clipboard,
                                 TerminalCopyOnSelectMode copyOnSelect);
