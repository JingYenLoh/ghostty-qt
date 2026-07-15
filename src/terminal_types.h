#pragma once

#include <QColor>
#include <QMetaType>
#include <QString>
#include <QVector>

#include <cstdint>

struct TerminalCell {
    QString text;
    QColor foreground;
    QColor background;
    QColor underlineColor;
    bool bold = false;
    bool italic = false;
    bool faint = false;
    bool underline = false;
    bool strikeThrough = false;
    bool overline = false;
    bool selected = false;
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
    bool cursorVisible = false;
    bool cursorBlinking = false;
    int cursorColumn = 0;
    int cursorRow = 0;
    int cursorStyle = 1;
    int cursorColumnSpan = 1;
    quint64 scrollTotal = 0;
    quint64 scrollOffset = 0;
    quint64 scrollLength = 0;
};

struct TerminalKeyInput {
    int key = 0;
    int modifiers = 0;
    QString text;
    bool pressed = true;
    bool autoRepeat = false;
    bool composing = false;
    uint32_t unshiftedCodepoint = 0;
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
Q_DECLARE_METATYPE(TerminalKeyInput)
Q_DECLARE_METATYPE(TerminalMouseInput)
