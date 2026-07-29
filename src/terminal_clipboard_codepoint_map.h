#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <variant>

// Preserve Ghostty's finalized, ordered clipboard replacement list. A u21
// replacement may not be a Unicode scalar; Ghostty emits U+FFFD for those
// values when formatting plain text.
using TerminalClipboardCodepointReplacement = std::variant<quint32, QString>;

struct TerminalClipboardCodepointMapEntry {
    quint32 first = 0;
    quint32 last = 0;
    TerminalClipboardCodepointReplacement replacement = quint32{0};

    bool operator==(const TerminalClipboardCodepointMapEntry &) const = default;
};

using TerminalClipboardCodepointMap =
    QVector<TerminalClipboardCodepointMapEntry>;
