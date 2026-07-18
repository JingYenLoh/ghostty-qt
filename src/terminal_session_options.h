#pragma once

#include "terminal_appearance.h"

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <limits>

enum class ScrollbackLimitUnit {
    Lines,
    Bytes,
};

struct ScrollbackLimit {
    quint64 value = 10'000;
    ScrollbackLimitUnit unit = ScrollbackLimitUnit::Lines;

    bool operator==(const ScrollbackLimit &) const = default;
};

enum class TerminalCopyOnSelectMode : quint8 {
    Disabled,
    Primary,
    PrimaryAndClipboard,
};

enum class TerminalClipboardDestination : quint8 {
    Standard,
    Primary,
    PrimaryAndStandard,
};

struct TerminalSelectionClipboardOptions {
    bool trimTrailingSpaces = true;
    TerminalCopyOnSelectMode copyOnSelect = TerminalCopyOnSelectMode::Primary;
    bool clearOnTyping = true;
    bool clearOnCopy = false;

    bool operator==(const TerminalSelectionClipboardOptions &) const = default;
};

// Paste safety is worker-owned because the decision depends on the terminal's
// live bracketed-paste mode as well as live-reloaded configuration.
struct TerminalClipboardPasteOptions {
    bool protection = true;
    bool bracketedSafe = true;

    bool operator==(const TerminalClipboardPasteOptions &) const = default;
};

// Value-only settings that an existing terminal session can apply. Keep this
// boundary limited to state that SessionWorker actually owns at runtime.
struct TerminalSessionRuntimeOptions {
    TerminalAppearance appearance;
    TerminalSelectionClipboardOptions selectionClipboard;
    TerminalClipboardPasteOptions clipboardPaste;
    bool linkUrl = true;

    bool operator==(const TerminalSessionRuntimeOptions &) const = default;
};

// One-time process and terminal construction settings. Runtime state is
// composed here so initialization and reload use the same representation.
struct TerminalSessionLaunchOptions {
    QString workingDirectory;
    QStringList program;
    ScrollbackLimit scrollbackLimit;
    bool hold = false;
    TerminalSessionRuntimeOptions runtime;

    bool operator==(const TerminalSessionLaunchOptions &) const = default;
};

// libghostty's max_scrollback is byte-valued. Preserve Ghostty config bytes
// exactly; convert the legacy line CLI using a conservative storage estimate
// of max(256, columns * 16) bytes per row, with saturating arithmetic.
inline quint64 scrollbackLimitInBytes(ScrollbackLimit limit, int columns)
{
    if (limit.unit == ScrollbackLimitUnit::Bytes) {
        return limit.value;
    }
    const quint64 boundedColumns = static_cast<quint64>(std::max(1, columns));
    constexpr quint64 EstimatedBytesPerCell = 16;
    constexpr quint64 MinimumEstimatedRowBytes = 256;
    const quint64 rowBytes = std::max(
        MinimumEstimatedRowBytes,
        boundedColumns * EstimatedBytesPerCell);
    if (limit.value > std::numeric_limits<quint64>::max() / rowBytes) {
        return std::numeric_limits<quint64>::max();
    }
    return limit.value * rowBytes;
}

Q_DECLARE_METATYPE(TerminalSessionLaunchOptions)
Q_DECLARE_METATYPE(TerminalSessionRuntimeOptions)
Q_DECLARE_METATYPE(TerminalClipboardDestination)
