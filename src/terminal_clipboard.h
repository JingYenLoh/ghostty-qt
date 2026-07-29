#pragma once

#include "terminal_session_options.h"
#include "terminal_types.h"

#include <QByteArrayView>
#include <QString>
#include <QtGlobal>

#include <optional>

class QClipboard;

enum class TerminalClipboardSource : quint8 {
    Standard,
    Primary,
};

// A terminal-originated explicit destination never falls back to another
// clipboard. Qt exposes both Ghostty's selection and primary locations through
// the X11/Wayland selection clipboard.
constexpr std::optional<TerminalClipboardSource>
terminalClipboardWriteTarget(TerminalClipboardLocation location,
                             bool supportsPrimary)
{
    switch (location) {
    case TerminalClipboardLocation::Standard:
        return TerminalClipboardSource::Standard;
    case TerminalClipboardLocation::Selection:
    case TerminalClipboardLocation::Primary:
        if (supportsPrimary) {
            return TerminalClipboardSource::Primary;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

inline bool validTerminalClipboardMimeType(QByteArrayView mimeType) noexcept
{
    if (mimeType.isEmpty()) {
        return false;
    }
    for (const char byte : mimeType) {
        if (byte == '\0') {
            return false;
        }
    }
    return true;
}

inline bool
validTerminalClipboardWritePayload(const TerminalClipboardWrite &write) noexcept
{
    for (const TerminalClipboardMimeRepresentation &content : write.contents) {
        if (!validTerminalClipboardMimeType(content.mime)) {
            return false;
        }
    }
    return true;
}

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
TerminalClipboardWriteTargets
writeTerminalClipboard(QClipboard *clipboard, const QString &text,
                       TerminalClipboardDestination destination);
// Commits all normalized MIME representations in one ownership transition.
// An empty contents vector clears the requested clipboard, while a
// representation with empty data remains an explicitly present format.
[[nodiscard]] bool writeTerminalClipboard(QClipboard *clipboard,
                                          const TerminalClipboardWrite &write);
// A present empty string is distinct from a clipboard without a text MIME
// representation. Explicit primary reads never fall back to the standard
// clipboard; middle-click fallback remains a separate policy below.
std::optional<QString> readTerminalClipboard(QClipboard *clipboard,
                                             TerminalClipboardSource source);
QString readMiddleClickClipboard(QClipboard *clipboard,
                                 TerminalCopyOnSelectMode copyOnSelect);
