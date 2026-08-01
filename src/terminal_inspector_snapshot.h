#pragma once

#include <QByteArray>
#include <QColor>
#include <QMetaType>
#include <QString>
#include <QVector>
#include <QtTypes>

// Owned values copied from libghostty-vt on the session thread. Keeping this
// contract independent of the C API prevents borrowed handles and upstream
// enum layouts from crossing the queued Qt boundary.
enum class TerminalInspectorScreen : quint8 {
    Primary,
    Alternate,
};

enum class TerminalInspectorStatus : quint8 {
    Unavailable,
    Ready,
    Failed,
};

struct TerminalInspectorModeState {
    QString name;
    quint16 number = 0;
    bool ansi = false;
    bool enabled = false;

    friend bool operator==(const TerminalInspectorModeState &,
                           const TerminalInspectorModeState &) = default;
};

struct TerminalInspectorSnapshot {
    TerminalInspectorStatus status = TerminalInspectorStatus::Unavailable;
    quint64 contentRevision = 0;

    quint16 columns = 0;
    quint16 rows = 0;
    quint16 cursorColumn = 0;
    quint16 cursorRow = 0;
    bool cursorPendingWrap = false;
    bool cursorVisible = false;
    TerminalInspectorScreen activeScreen = TerminalInspectorScreen::Primary;
    bool viewportActive = true;
    bool terminalMouseTracking = false;
    quint64 totalRows = 0;
    quint64 scrollbackRows = 0;
    quint64 scrollTotal = 0;
    quint64 scrollOffset = 0;
    quint64 scrollLength = 0;
    quint32 widthPixels = 0;
    quint32 heightPixels = 0;

    QColor effectiveForeground;
    QColor effectiveBackground;
    QColor effectiveCursor;
    QColor defaultForeground;
    QColor defaultBackground;
    QColor defaultCursor;
    QVector<QColor> effectivePalette;
    QVector<QColor> defaultPalette;

    quint8 kittyKeyboardFlags = 0;
    bool kittyGraphicsAvailable = false;
    quint64 kittyImageStorageLimitBytes = 0;
    bool kittyFileMedium = false;
    bool kittyTemporaryFileMedium = false;
    bool kittySharedMemoryMedium = false;

    QVector<TerminalInspectorModeState> modes;

    friend bool operator==(const TerminalInspectorSnapshot &,
                           const TerminalInspectorSnapshot &) = default;
};

enum class TerminalInspectorCellStatus : quint8 {
    Unavailable,
    Ready,
    Stale,
    OutOfBounds,
    Failed,
};

enum class TerminalInspectorCellContentKind : quint8 {
    Codepoint,
    Grapheme,
    BackgroundPalette,
    BackgroundRgb,
};

enum class TerminalInspectorCellWidthRole : quint8 {
    Narrow,
    Wide,
    SpacerTail,
    SpacerHead,
};

enum class TerminalInspectorCellSemantic : quint8 {
    Output,
    Input,
    Prompt,
};

enum class TerminalInspectorRowSemantic : quint8 {
    None,
    Prompt,
    PromptContinuation,
};

enum class TerminalInspectorStyleColorKind : quint8 {
    None,
    Palette,
    Rgb,
};

enum class TerminalInspectorUnderlineStyle : quint8 {
    None,
    Single,
    Double,
    Curly,
    Dotted,
    Dashed,
};

struct TerminalInspectorStyleColor {
    TerminalInspectorStyleColorKind kind =
        TerminalInspectorStyleColorKind::None;
    int paletteIndex = -1;
    QColor rgb;

    friend bool operator==(const TerminalInspectorStyleColor &,
                           const TerminalInspectorStyleColor &) = default;
};

struct TerminalInspectorCellStyle {
    TerminalInspectorStyleColor foreground;
    TerminalInspectorStyleColor background;
    TerminalInspectorStyleColor underlineColor;
    bool bold = false;
    bool italic = false;
    bool faint = false;
    bool blink = false;
    bool inverse = false;
    bool invisible = false;
    bool strikethrough = false;
    bool overline = false;
    TerminalInspectorUnderlineStyle underline =
        TerminalInspectorUnderlineStyle::None;

    friend bool operator==(const TerminalInspectorCellStyle &,
                           const TerminalInspectorCellStyle &) = default;
};

// A one-shot, owned copy of public libghostty-vt data for one viewport cell.
// The worker rejects a request when the GUI frame revision that supplied its
// coordinate is no longer current, rather than silently inspecting a new cell
// that has moved under the same viewport coordinate.
struct TerminalInspectorCellSnapshot {
    TerminalInspectorCellStatus status =
        TerminalInspectorCellStatus::Unavailable;
    quint64 contentRevision = 0;
    int viewportColumn = -1;
    int viewportRow = -1;
    TerminalInspectorScreen activeScreen = TerminalInspectorScreen::Primary;

    QString text;
    QVector<quint32> codepoints;
    TerminalInspectorCellContentKind contentKind =
        TerminalInspectorCellContentKind::Codepoint;
    TerminalInspectorCellWidthRole widthRole =
        TerminalInspectorCellWidthRole::Narrow;
    bool hasText = false;
    bool hasStyling = false;
    quint16 styleId = 0;
    bool hasHyperlink = false;
    bool protectedCell = false;
    TerminalInspectorCellSemantic semantic =
        TerminalInspectorCellSemantic::Output;
    QByteArray hyperlinkUri;

    // Set only for the background-only content tag variants.
    TerminalInspectorStyleColor contentBackground;
    TerminalInspectorCellStyle style;

    bool rowWrapped = false;
    bool rowWrapContinuation = false;
    TerminalInspectorRowSemantic rowSemantic =
        TerminalInspectorRowSemantic::None;

    friend bool operator==(const TerminalInspectorCellSnapshot &,
                           const TerminalInspectorCellSnapshot &) = default;
};

Q_DECLARE_METATYPE(TerminalInspectorScreen)
Q_DECLARE_METATYPE(TerminalInspectorStatus)
Q_DECLARE_METATYPE(TerminalInspectorModeState)
Q_DECLARE_METATYPE(TerminalInspectorSnapshot)
Q_DECLARE_METATYPE(TerminalInspectorCellSnapshot)
