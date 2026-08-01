#pragma once

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

Q_DECLARE_METATYPE(TerminalInspectorScreen)
Q_DECLARE_METATYPE(TerminalInspectorStatus)
Q_DECLARE_METATYPE(TerminalInspectorModeState)
Q_DECLARE_METATYPE(TerminalInspectorSnapshot)
