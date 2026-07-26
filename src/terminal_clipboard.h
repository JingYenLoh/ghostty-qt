#pragma once

#include "terminal_session_options.h"

#include <QString>
#include <QtGlobal>

#include <optional>

class QClipboard;

enum class TerminalClipboardSource : quint8 {
    Standard,
    Primary,
};

struct TerminalClipboardWriteTargets {
    bool standard = false;
    bool primary = false;
};

constexpr TerminalClipboardWriteTargets
terminalClipboardWriteTargets(TerminalClipboardDestination destination,
                              bool supportsPrimary)
{
    switch (destination) {
    case TerminalClipboardDestination::Standard: return {.standard = true};
    case TerminalClipboardDestination::Primary:
        return supportsPrimary
            ? TerminalClipboardWriteTargets{.primary = true}
            : TerminalClipboardWriteTargets{.standard = true};
    case TerminalClipboardDestination::PrimaryAndStandard:
        return {.standard = true, .primary = supportsPrimary};
    }
    return {};
}

constexpr TerminalClipboardSource
terminalMiddleClickSource(TerminalCopyOnSelectMode copyOnSelect,
                          bool supportsPrimary)
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
// A present empty string is distinct from a clipboard without a text MIME
// representation. Explicit primary reads never fall back to the standard
// clipboard; middle-click fallback remains a separate policy below.
std::optional<QString> readTerminalClipboard(QClipboard *clipboard,
                                             TerminalClipboardSource source);
QString readMiddleClickClipboard(QClipboard *clipboard,
                                 TerminalCopyOnSelectMode copyOnSelect);
