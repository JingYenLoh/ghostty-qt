#pragma once

#include <QMetaType>
#include <QtGlobal>

// Viewport requests remain value-only across the UI/session thread boundary.
// Row uses the same absolute full-screen coordinate space as libghostty's
// scrollbar offset; Selection resolves the current selection atomically on
// the session thread so no untracked Ghostty grid reference can escape.
struct TerminalViewportRequest {
    enum class Kind : quint8 {
        Top,
        Bottom,
        Delta,
        Row,
        Selection,
    };

    Kind kind = Kind::Delta;
    qint64 delta = 0;
    quint64 row = 0;

    bool operator==(const TerminalViewportRequest &) const = default;
};

enum class TerminalSelectionAdjustment : quint8 {
    Left,
    Right,
    Up,
    Down,
    PageUp,
    PageDown,
    Home,
    End,
    BeginningOfLine,
    EndOfLine,
};

Q_DECLARE_METATYPE(TerminalViewportRequest)
Q_DECLARE_METATYPE(TerminalSelectionAdjustment)
