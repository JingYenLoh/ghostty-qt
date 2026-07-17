#pragma once

#include "terminal_actions.h"

#include <QColor>
#include <QMetaType>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdint>

// The renderer needs the original SGR foreground source in addition to the
// resolved RGB value. Ghostty's bold-color=bright behavior only promotes the
// first eight palette entries; direct RGB and the remaining palette entries
// must retain their color.
enum class TerminalColorSource : quint8 {
    Default,
    Palette,
    Rgb,
};

enum class TerminalUnderlineStyle : quint8 {
    None,
    Single,
    Double,
    Curly,
    Dotted,
    Dashed,
};

// Worker-owned hyperlink anchors can remain meaningful while their cell is
// temporarily outside the viewport or belongs to the inactive screen. Keep
// that distinct from permanent invalidation so a hover can reappear without
// rescanning when the tracked cell becomes visible again.
enum class TerminalHyperlinkState : quint8 {
    Invalid,
    // The viewport coordinate came from an older frame. This is retryable
    // once the UI installs the worker revision returned with the result.
    Stale,
    Hidden,
    Visible,
};

struct TerminalCell {
    QString text;
    QColor foreground;
    QColor background;
    QColor underlineColor;
    TerminalColorSource styleForegroundSource = TerminalColorSource::Default;
    int styleForegroundPaletteIndex = -1;
    bool bold = false;
    bool italic = false;
    bool faint = false;
    // Retained for semantic parity. The pinned Ghostty renderer currently
    // records SGR blink but deliberately does not animate text with it.
    bool textBlink = false;
    bool inverse = false;
    bool invisible = false;
    bool underlineUsesForeground = true;
    TerminalUnderlineStyle underlineStyle = TerminalUnderlineStyle::None;
    bool strikeThrough = false;
    bool overline = false;
    bool selected = false;
    // The URI remains worker-owned and is resolved only for an active hover.
    // This cheap bit lets the UI avoid querying ordinary cells.
    bool hasHyperlink = false;
    bool spacer = false;
    int columnSpan = 1;
};

struct TerminalFrame {
    int columns = 0;
    int rows = 0;
    QVector<TerminalCell> cells;
    QColor foreground = QColor(QStringLiteral("#d8dee9"));
    QColor background = QColor(QStringLiteral("#1e222a"));
    QColor cursorColor = QColor(QStringLiteral("#d8dee9"));
    QVector<QColor> palette;
    bool cursorColorExplicit = false;
    bool cursorVisible = false;
    bool cursorBlinking = false;
    int cursorColumn = 0;
    int cursorRow = 0;
    int cursorStyle = 1;
    int cursorColumnSpan = 1;
    quint64 scrollTotal = 0;
    quint64 scrollOffset = 0;
    quint64 scrollLength = 0;
    // Monotonic worker-owned revision for terminal content and viewport
    // mutations. New hyperlink anchors require the exact frame revision that
    // supplied their initial viewport coordinate; accepted anchors then follow
    // the logical cell independently through later revisions.
    quint64 contentRevision = 0;
};

// A row is the smallest cell payload that crosses the worker/UI thread
// boundary after the initial frame. The row index is viewport-relative.
struct TerminalRowUpdate {
    int row = 0;
    QVector<TerminalCell> cells;
};

// Value-only delta produced from libghostty's render state. A full update is
// the fallback used for the first frame and whenever the viewport shape or
// global render state changes. Partial updates contain only dirty rows and
// independently identify non-cell visual changes.
struct TerminalUpdate {
    int columns = 0;
    int rows = 0;
    bool fullFrame = false;
    QVector<TerminalRowUpdate> dirtyRows;

    bool colorsChanged = false;
    QColor foreground = QColor(QStringLiteral("#d8dee9"));
    QColor background = QColor(QStringLiteral("#1e222a"));
    QColor cursorColor = QColor(QStringLiteral("#d8dee9"));
    QVector<QColor> palette;
    bool cursorColorExplicit = false;

    bool cursorChanged = false;
    bool cursorVisible = false;
    bool cursorBlinking = false;
    int cursorColumn = 0;
    int cursorRow = 0;
    int cursorStyle = 1;
    int cursorColumnSpan = 1;

    bool scrollbarChanged = false;
    quint64 scrollTotal = 0;
    quint64 scrollOffset = 0;
    quint64 scrollLength = 0;

    quint64 contentRevision = 0;

    // SessionWorker sets this only for PTY output activity. It is transport
    // metadata rather than terminal state and therefore is not retained in
    // TerminalFrame.
    bool resetCursorBlink = false;

    bool hasChanges() const
    {
        return fullFrame || !dirtyRows.isEmpty() || colorsChanged
            || cursorChanged || scrollbarChanged || resetCursorBlink;
    }
};

// Applies an update without exposing terminal handles to the UI. Validation
// happens before mutation so a malformed or incomplete delta cannot leave the
// retained frame half-updated. Returns false for an invalid update or a
// partial update whose dimensions do not match the retained frame.
inline bool applyTerminalUpdate(TerminalFrame *frame, const TerminalUpdate &update)
{
    if (frame == nullptr || update.columns <= 0 || update.rows <= 0) {
        return false;
    }

    QVector<bool> seen(update.rows, false);
    for (const TerminalRowUpdate &row : update.dirtyRows) {
        if (row.row < 0 || row.row >= update.rows || seen.at(row.row)
            || row.cells.size() != update.columns) {
            return false;
        }
        seen[row.row] = true;
    }
    if (update.fullFrame) {
        if (update.dirtyRows.size() != update.rows) {
            return false;
        }
    } else if (frame->columns != update.columns || frame->rows != update.rows
               || frame->cells.size() != update.columns * update.rows) {
        return false;
    }

    if (update.fullFrame) {
        frame->columns = update.columns;
        frame->rows = update.rows;
        frame->cells.resize(update.columns * update.rows);
    }
    for (const TerminalRowUpdate &row : update.dirtyRows) {
        const qsizetype destination = static_cast<qsizetype>(row.row) * update.columns;
        std::copy(row.cells.cbegin(), row.cells.cend(), frame->cells.begin() + destination);
    }

    if (update.fullFrame || update.colorsChanged) {
        frame->foreground = update.foreground;
        frame->background = update.background;
        frame->cursorColor = update.cursorColor;
        frame->palette = update.palette;
        frame->cursorColorExplicit = update.cursorColorExplicit;
    }
    if (update.fullFrame || update.cursorChanged) {
        frame->cursorVisible = update.cursorVisible;
        frame->cursorBlinking = update.cursorBlinking;
        frame->cursorColumn = update.cursorColumn;
        frame->cursorRow = update.cursorRow;
        frame->cursorStyle = update.cursorStyle;
        frame->cursorColumnSpan = update.cursorColumnSpan;
    }
    if (update.fullFrame || update.scrollbarChanged) {
        frame->scrollTotal = update.scrollTotal;
        frame->scrollOffset = update.scrollOffset;
        frame->scrollLength = update.scrollLength;
    }
    frame->contentRevision = update.contentRevision;
    return true;
}

struct TerminalKeyInput {
    int key = 0;
    int modifiers = 0;
    QString text;
    // Qt Wayland/X11 expose XKB keycodes here (Linux evdev code + 8).
    quint32 nativeScanCode = 0;
    bool pressed = true;
    bool autoRepeat = false;
    bool composing = false;
    uint32_t unshiftedCodepoint = 0;
};

// Resolves terminal input held while the UI decides whether a key sequence
// matches. Leaders are encoded on the session thread when staged so later VT
// mode changes cannot alter the bytes they would originally have produced.
enum class TerminalSequenceResolution : quint8 {
    Drop,
    Flush,
    FlushAndSendCurrent,
};

struct TerminalMouseInput {
    enum Action {
        Press,
        Release,
        Motion,
    };

    Action action = Motion;
    int button = 0;
    int modifiers = 0;
    float x = 0.0F;
    float y = 0.0F;
    bool anyButtonPressed = false;
};

Q_DECLARE_METATYPE(TerminalFrame)
Q_DECLARE_METATYPE(TerminalUpdate)
Q_DECLARE_METATYPE(TerminalHyperlinkState)
Q_DECLARE_METATYPE(TerminalKeyInput)
Q_DECLARE_METATYPE(TerminalSequenceResolution)
Q_DECLARE_METATYPE(TerminalMouseInput)
