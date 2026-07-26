#pragma once

#include "terminal_appearance.h"

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <limits>
#include <optional>

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

// Frontend-owned scheduling for construction of a terminal session. Deferred
// mode is used only while an initially non-windowed Qt surface waits for its
// compositor-assigned viewport; it is not part of the worker launch payload.
enum class TerminalSessionStartMode : quint8 {
    Immediate,
    Deferred,
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
    QVector<quint32> selectionWordChars;
    quint32 clickRepeatIntervalMilliseconds = 500;
    TerminalClipboardPasteOptions clipboardPaste;
    bool linkUrl = true;

    bool operator==(const TerminalSessionRuntimeOptions &) const = default;
};

// Complete value needed to construct libghostty and the PTY at the same
// geometry. The optional launch-time instance is consumed before either is
// created; later resizes use this representation after the GUI has laid out
// the pane.
struct TerminalSessionGeometry {
    int columns = 80;
    int rows = 24;
    int cellWidthPixels = 8;
    int cellHeightPixels = 16;
    int surfaceWidthPixels = 640;
    int surfaceHeightPixels = 384;

    bool operator==(const TerminalSessionGeometry &) const = default;
};

inline TerminalSessionGeometry
normalizedTerminalSessionGeometry(TerminalSessionGeometry geometry) noexcept
{
    constexpr int maximumCells =
        static_cast<int>(std::numeric_limits<quint16>::max());
    geometry.columns = std::clamp(geometry.columns, 1, maximumCells);
    geometry.rows = std::clamp(geometry.rows, 1, maximumCells);
    geometry.cellWidthPixels = std::max(geometry.cellWidthPixels, 1);
    geometry.cellHeightPixels = std::max(geometry.cellHeightPixels, 1);
    geometry.surfaceWidthPixels = std::max(geometry.surfaceWidthPixels, 1);
    geometry.surfaceHeightPixels = std::max(geometry.surfaceHeightPixels, 1);
    return geometry;
}

// One-time process and terminal construction settings. Runtime state is
// composed here so initialization and reload use the same representation.
struct TerminalSessionLaunchOptions {
    QString workingDirectory;
    bool inheritWorkingDirectory = false;
    QStringList program;
    ScrollbackLimit scrollbackLimit;
    bool hold = false;
    // Frontend window dimensions never enter the reusable LaunchOptions
    // projection. The workspace supplies this one-shot value only for its
    // first pane so tabs and splits cannot inherit window-level geometry.
    std::optional<TerminalSessionGeometry> initialGeometry;
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
    const quint64 rowBytes = std::max(MinimumEstimatedRowBytes,
                                      boundedColumns * EstimatedBytesPerCell);
    if (limit.value > std::numeric_limits<quint64>::max() / rowBytes) {
        return std::numeric_limits<quint64>::max();
    }
    return limit.value * rowBytes;
}

Q_DECLARE_METATYPE(TerminalSessionLaunchOptions)
Q_DECLARE_METATYPE(TerminalSessionRuntimeOptions)
Q_DECLARE_METATYPE(TerminalClipboardDestination)
