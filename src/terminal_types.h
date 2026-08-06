#pragma once

#include "terminal_actions.h"
#include "terminal_kitty_graphics.h"

#include <QBitArray>
#include <QByteArray>
#include <QColor>
#include <QMetaType>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <compare>
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

// OSC 8 and regex-detected links share the same hover and activation
// machinery, but opening a regex match may first resolve a relative path
// against the terminal working directory.
enum class TerminalLinkKind : quint8 {
    Osc8,
    Regex,
};

enum class TerminalSearchDirection : quint8 {
    Next,
    Previous,
};

// Search results use full-screen coordinates so they remain independent of
// the current viewport. They are value-only snapshots, not libghostty grid
// references, and are valid only for the terminal revision that produced
// them.
struct TerminalSearchCell {
    quint16 column = 0;
    quint32 screenRow = 0;

    friend constexpr std::strong_ordering
    operator<=>(const TerminalSearchCell &left, const TerminalSearchCell &right)
    {
        if (const auto rowOrder = left.screenRow <=> right.screenRow;
            rowOrder != 0) {
            return rowOrder;
        }
        return left.column <=> right.column;
    }

    friend bool operator==(const TerminalSearchCell &,
                           const TerminalSearchCell &) = default;
};

struct TerminalSearchRange {
    TerminalSearchCell start;
    TerminalSearchCell end;

    friend bool operator==(const TerminalSearchRange &,
                           const TerminalSearchRange &) = default;
};

struct TerminalSearchUpdate {
    quint64 generation = 0;
    quint64 contentRevision = 0;
    bool active = false;
    bool complete = false;
    quint64 scannedRows = 0;
    quint64 totalRows = 0;
    quint64 totalMatches = 0;
    qint64 selectedMatch = -1;
    int columns = 0;
    int rows = 0;
    // Row-major viewport masks. For active searches, the worker sizes both
    // masks to columns * rows; inactive updates leave them empty. The visible
    // mask may include independently probed viewport candidates whose
    // canonical index is not reflected in totalMatches yet. The selected mask
    // is canonical.
    QBitArray visibleCellMask;
    QBitArray selectedCellMask;
};

struct TerminalCell {
    QString text;
    // Preserve the terminal's authoritative base cell content separately
    // from its UTF-16 grapheme. Renderer shaping rules must distinguish a
    // plain `f` from a grapheme that merely begins with `f`.
    quint32 baseCodepoint = 0;
    bool plainCodepoint = false;
    // Ghostty's cursor shaping rule keeps cells with extra grapheme
    // codepoints joined, but still breaks around plain and empty cells.
    bool extendedGrapheme = false;
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
    // True when libghostty reports a cell-owned background source. This
    // provenance cannot be recovered from the resolved RGB value because an
    // explicit color may equal the terminal's global background. Keep it
    // independent of inverse, selection, and search presentation policy.
    bool backgroundExplicit = false;
    // Derived from Ghostty's raw base codepoint, not from shaped QString
    // contents. Only the glyph is exempt; decorations still use contrast.
    bool minimumContrastExemptGlyph = false;
    // The URI remains worker-owned and is resolved only for an active hover.
    // This cheap bit lets the UI avoid querying ordinary cells.
    bool hasHyperlink = false;
    bool spacer = false;
    int columnSpan = 1;
};

struct TerminalRowPresentation {
    // Ghostty's conservative vertical-padding heuristic rejects prompt rows,
    // default backgrounds, default-colored explicit backgrounds, and
    // perfect-fit powerline glyphs.
    bool paddingExtensionSafe = false;

    bool operator==(const TerminalRowPresentation &) const = default;
};

struct TerminalFrame {
    int columns = 0;
    int rows = 0;
    QVector<TerminalCell> cells;
    QVector<TerminalRowPresentation> rowPresentation;
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
    std::shared_ptr<const TerminalKittyGraphicsSnapshot> kittyGraphics;
    // Monotonic worker-owned revision for terminal content and viewport
    // mutations. New hyperlink anchors require the exact frame revision that
    // supplied their initial viewport coordinate; accepted anchors then follow
    // the logical cell independently through later revisions.
    quint64 contentRevision = 0;
};

// A row is the smallest cell payload that crosses the worker/UI thread
// boundary after the initial frame. The row index is viewport-relative, and
// updates carry rows in strictly increasing order.
struct TerminalRowUpdate {
    int row = 0;
    QVector<TerminalCell> cells;
    TerminalRowPresentation presentation;
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

    bool kittyGraphicsChanged = false;
    std::shared_ptr<const TerminalKittyGraphicsSnapshot> kittyGraphics;

    quint64 contentRevision = 0;

    // SessionWorker sets this only for PTY output activity. It is transport
    // metadata rather than terminal state and therefore is not retained in
    // TerminalFrame.
    bool resetCursorBlink = false;

    bool hasChanges() const
    {
        return fullFrame || !dirtyRows.isEmpty() || colorsChanged
            || cursorChanged || scrollbarChanged || kittyGraphicsChanged
            || resetCursorBlink;
    }
};

// Terminal-originated clipboard operations are normalized by libghostty
// before crossing this boundary. Keep every representation owned and
// binary-safe because the source callback lends its storage only for the
// callback duration.
enum class TerminalClipboardLocation : quint8 {
    Standard,
    Selection,
    Primary,
};

struct TerminalClipboardMimeRepresentation {
    QByteArray mime;
    QByteArray data;

    friend bool
    operator==(const TerminalClipboardMimeRepresentation &,
               const TerminalClipboardMimeRepresentation &) = default;
};

struct TerminalClipboardWrite {
    TerminalClipboardLocation location = TerminalClipboardLocation::Standard;
    // An empty collection means clear the destination. A representation with
    // empty data is a distinct, explicit empty value.
    QVector<TerminalClipboardMimeRepresentation> contents;

    friend bool operator==(const TerminalClipboardWrite &,
                           const TerminalClipboardWrite &) = default;
};

struct TerminalClipboardWriteRequest {
    TerminalClipboardWrite write;
    // This snapshots the live access policy at the point the escape sequence
    // was consumed, preserving FIFO behavior across later config reloads.
    bool confirmationRequired = false;

    friend bool operator==(const TerminalClipboardWriteRequest &,
                           const TerminalClipboardWriteRequest &) = default;
};

// Validates the value-only shape shared by every retained view of a render
// update. Keeping one representability ceiling prevents the GUI frame and
// worker-side indexes from accepting different viewport dimensions.
[[nodiscard]] inline bool validTerminalUpdateShape(const TerminalUpdate &update)
{
    if (update.columns <= 0 || update.rows <= 0) {
        return false;
    }

    const qsizetype columnCount = update.columns;
    const qsizetype rowCount = update.rows;
    if (columnCount > QVector<TerminalCell>::maxSize() / rowCount) {
        return false;
    }
    int previousRow = -1;
    for (const TerminalRowUpdate &row : update.dirtyRows) {
        if (row.row <= previousRow || row.row >= update.rows
            || row.cells.size() != update.columns) {
            return false;
        }
        previousRow = row.row;
    }
    if (update.fullFrame) {
        if (update.dirtyRows.size() != update.rows) {
            return false;
        }
    }
    return true;
}

// Applies an update without exposing terminal handles to the UI. Validation
// happens before mutation so a malformed or incomplete delta cannot leave the
// retained frame half-updated. Returns false for an invalid update or a
// partial update whose dimensions do not match the retained frame.
[[nodiscard]] inline bool applyTerminalUpdate(TerminalFrame &frame,
                                              const TerminalUpdate &update)
{
    if (!validTerminalUpdateShape(update)) {
        return false;
    }
    const qsizetype cellCount =
        static_cast<qsizetype>(update.columns) * update.rows;
    if (!update.fullFrame
        && (frame.columns != update.columns || frame.rows != update.rows
            || frame.cells.size() != cellCount)) {
        return false;
    }

    if (update.fullFrame) {
        frame.columns = update.columns;
        frame.rows = update.rows;
        frame.cells.resize(cellCount);
        frame.rowPresentation.resize(update.rows);
    } else if (frame.rowPresentation.size() != update.rows) {
        frame.rowPresentation.resize(update.rows);
    }
    for (const TerminalRowUpdate &row : update.dirtyRows) {
        const qsizetype destination =
            static_cast<qsizetype>(row.row) * update.columns;
        std::copy(row.cells.cbegin(), row.cells.cend(),
                  frame.cells.begin() + destination);
        frame.rowPresentation[row.row] = row.presentation;
    }

    if (update.fullFrame || update.colorsChanged) {
        frame.foreground = update.foreground;
        frame.background = update.background;
        frame.cursorColor = update.cursorColor;
        frame.palette = update.palette;
        frame.cursorColorExplicit = update.cursorColorExplicit;
    }
    if (update.fullFrame || update.cursorChanged) {
        frame.cursorVisible = update.cursorVisible;
        frame.cursorBlinking = update.cursorBlinking;
        frame.cursorColumn = update.cursorColumn;
        frame.cursorRow = update.cursorRow;
        frame.cursorStyle = update.cursorStyle;
        frame.cursorColumnSpan = update.cursorColumnSpan;
    }
    if (update.fullFrame || update.scrollbarChanged) {
        frame.scrollTotal = update.scrollTotal;
        frame.scrollOffset = update.scrollOffset;
        frame.scrollLength = update.scrollLength;
    }
    if (update.fullFrame || update.kittyGraphicsChanged) {
        frame.kittyGraphics = update.kittyGraphics;
    }
    frame.contentRevision = update.contentRevision;
    return true;
}

struct TerminalKeyInput {
    int key = 0;
    int modifiers = 0;
    // Modifiers used by the active keyboard layout to produce `text`.
    // libghostty removes these when deciding whether printable input is
    // modified, while retaining the complete physical modifier state for
    // Kitty report-all mode.
    int consumedModifiers = 0;
    QString text;
    // Qt Wayland/X11 expose XKB keycodes here (Linux evdev code + 8).
    quint32 nativeScanCode = 0;
    bool pressed = true;
    bool autoRepeat = false;
    bool composing = false;
    uint32_t unshiftedCodepoint = 0;
    // Zero keeps ordinary input free of diagnostic work. A non-zero pair is
    // allocated only while this pane's inspector is actively capturing; it is
    // semantically inert to libghostty and lets the worker's bounded encoding
    // outcome correlate with the frontend decision that produced this input.
    quint64 inspectorTraceGeneration = 0;
    quint64 inspectorTraceId = 0;
};

enum class TerminalKeyboardTraceDecisionKind : quint8 {
    RootApplicationBinding,
    RootGlobalBinding,
    RootConsumedRelease,
    PaneUnmatched,
    PaneLeader,
    PaneInvalidSequence,
    PaneIgnoredSequence,
    PaneLocalBinding,
    PaneBroadBinding,
    PaneFallbackConsumed,
    PaneFallbackPassed,
    PaneConsumedRelease,
    PaneInspectorCancel,
};

// GUI-thread-only projection of one actual matcher decision. This is emitted
// only while inspector capture is active; the model performs bounded escaping
// after checking its pause state, and no trie traversal is repeated solely for
// diagnostics.
struct TerminalKeyboardTraceDecision {
    TerminalKeyInput input;
    TerminalKeyboardTraceDecisionKind kind =
        TerminalKeyboardTraceDecisionKind::PaneUnmatched;
    quint64 sequenceToken = 0;
    QStringList actions;
    QStringList activeTables;
    QStringList pendingSequence;
    bool consumed = false;
    bool performable = false;
    bool all = false;
    bool global = false;
    bool physical = false;
};

enum class TerminalKeyboardTraceOperation : quint8 {
    Key,
    SequenceStage,
    SequenceResolution,
};

enum class TerminalKeyboardTraceDisposition : quint8 {
    Queued,
    EncoderFailed,
    EncoderEmpty,
    KeyboardActionMode,
    ReadOnly,
    TerminalUnavailable,
    SessionUnavailable,
    ExitWaitConsumed,
    Staged,
    Dropped,
    Superseded,
    StaleSequence,
};

// Worker-to-GUI diagnostic value. `encodedPrefix` is capped at its producer;
// encodedByteCount always describes the complete output so the inspector never
// needs to copy or hex-format an unbounded byte array.
struct TerminalKeyboardTraceResult {
    static constexpr qsizetype MaximumEncodedPrefix = 64;

    quint64 generation = 0;
    quint64 traceId = 0;
    quint64 sequenceToken = 0;
    TerminalKeyboardTraceOperation operation =
        TerminalKeyboardTraceOperation::Key;
    TerminalKeyboardTraceDisposition disposition =
        TerminalKeyboardTraceDisposition::TerminalUnavailable;
    qint64 encodedByteCount = 0;
    QByteArray encodedPrefix;
    bool prefixTruncated = false;

    friend bool operator==(const TerminalKeyboardTraceResult &,
                           const TerminalKeyboardTraceResult &) = default;
};

// One GUI input-method callback becomes one worker operation so committed
// bytes and selection lifecycle cannot interleave with clipboard commands.
struct TerminalInputMethodInput {
    QString commitText;
    bool preeditTransition = false;

    friend bool operator==(const TerminalInputMethodInput &,
                           const TerminalInputMethodInput &) = default;
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

// Whole wheel rows and columns cross the session boundary as one value so the
// worker can choose alternate-scroll, DEC mouse reporting, or viewport
// movement against one current terminal-state snapshot. The frontend policy
// remains explicit because it is pane-owned rather than a terminal mode.
struct TerminalWheelInput {
    qint64 rows = 0;
    qint64 columns = 0;
    int modifiers = 0;
    float x = 0.0F;
    float y = 0.0F;
    bool mouseReportingEnabled = true;

    friend bool operator==(const TerminalWheelInput &,
                           const TerminalWheelInput &) = default;
};

// A local right press is resolved on the session thread because selection
// containment, link matching, and copy-or-paste branching must observe one
// authoritative terminal state. The GUI retains only the popup position.
struct TerminalRightClickInput {
    quint64 requestId = 0;
    quint64 contentRevision = 0;
    int column = 0;
    int row = 0;
    int modifiers = 0;
    // Shift is removed from Ghostty link matching only when it was the
    // physical escape hatch from an otherwise captured DEC mouse gesture.
    bool shiftBypassedMouseCapture = false;

    friend bool operator==(const TerminalRightClickInput &,
                           const TerminalRightClickInput &) = default;
};

enum class TerminalRightClickEffect : quint8 {
    None,
    Paste,
    ContextMenu,
};

struct TerminalRightClickResult {
    quint64 requestId = 0;
    quint64 contentRevision = 0;
    TerminalRightClickEffect effect = TerminalRightClickEffect::None;
    bool selectionAvailable = false;

    friend bool operator==(const TerminalRightClickResult &,
                           const TerminalRightClickResult &) = default;
};

// Qt owns pointer hit testing and timestamp capture, but libghostty owns the
// stateful repeat-click classification. Keep the cross-thread payload
// value-only and explicit about coordinate and timestamp units.
struct TerminalSelectionPressInput {
    int column = 0;
    int row = 0;
    double surfaceX = 0.0;
    double surfaceY = 0.0;
    quint64 timestampNanoseconds = 0;
    bool timestampValid = false;
    // On Linux, Ghostty maps Ctrl triple-clicks to semantic command output
    // rather than the ordinary logical line.
    bool controlModifier = false;
    // A released Shift press may extend the retained gesture after the
    // repeat-click interval. The pane resolves mouse-shift-capture before
    // setting this semantic candidate; the worker owns the timing decision.
    bool extendExistingSelection = false;
    // Linux Ctrl+Alt on the extension press selects a rectangular range
    // immediately, just as the same modifiers on a later drag do.
    bool rectangular = false;

    friend bool operator==(const TerminalSelectionPressInput &,
                           const TerminalSelectionPressInput &) = default;
};

struct TerminalSelectionDragInput {
    int column = 0;
    int row = 0;
    double surfaceX = 0.0;
    double surfaceY = 0.0;
    bool rectangular = false;

    friend bool operator==(const TerminalSelectionDragInput &,
                           const TerminalSelectionDragInput &) = default;
};

Q_DECLARE_METATYPE(TerminalFrame)
Q_DECLARE_METATYPE(TerminalUpdate)
Q_DECLARE_METATYPE(TerminalClipboardLocation)
Q_DECLARE_METATYPE(TerminalClipboardMimeRepresentation)
Q_DECLARE_METATYPE(TerminalClipboardWrite)
Q_DECLARE_METATYPE(TerminalClipboardWriteRequest)
Q_DECLARE_METATYPE(TerminalHyperlinkState)
Q_DECLARE_METATYPE(TerminalLinkKind)
Q_DECLARE_METATYPE(TerminalSearchDirection)
Q_DECLARE_METATYPE(TerminalSearchCell)
Q_DECLARE_METATYPE(TerminalSearchRange)
Q_DECLARE_METATYPE(TerminalSearchUpdate)
Q_DECLARE_METATYPE(TerminalKeyInput)
Q_DECLARE_METATYPE(TerminalKeyboardTraceDecisionKind)
Q_DECLARE_METATYPE(TerminalKeyboardTraceDecision)
Q_DECLARE_METATYPE(TerminalKeyboardTraceOperation)
Q_DECLARE_METATYPE(TerminalKeyboardTraceDisposition)
Q_DECLARE_METATYPE(TerminalKeyboardTraceResult)
Q_DECLARE_METATYPE(TerminalInputMethodInput)
Q_DECLARE_METATYPE(TerminalSequenceResolution)
Q_DECLARE_METATYPE(TerminalMouseInput)
Q_DECLARE_METATYPE(TerminalWheelInput)
Q_DECLARE_METATYPE(TerminalRightClickInput)
Q_DECLARE_METATYPE(TerminalRightClickEffect)
Q_DECLARE_METATYPE(TerminalRightClickResult)
Q_DECLARE_METATYPE(TerminalSelectionPressInput)
Q_DECLARE_METATYPE(TerminalSelectionDragInput)
