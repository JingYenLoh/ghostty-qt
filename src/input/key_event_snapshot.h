#pragma once

#include <QEvent>
#include <QKeyEvent>
#include <QtGlobal>

#include <algorithm>
#include <limits>

// Owning copy of the value-only portion of a Qt key event. Configuration and
// sequence transactions use this to replay synchronous reentrant input only
// after every participating matcher and pane has reached one generation.
struct KeyEventSnapshot final {
    bool pressed = false;
    int key = 0;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    quint32 nativeScanCode = 0;
    quint32 nativeVirtualKey = 0;
    quint32 nativeModifiers = 0;
    QString text;
    bool autoRepeat = false;
    int count = 1;

    [[nodiscard]] static KeyEventSnapshot capture(const QKeyEvent &event)
    {
        Q_ASSERT(event.type() == QEvent::KeyPress
                 || event.type() == QEvent::KeyRelease);
        return {
            .pressed = event.type() == QEvent::KeyPress,
            .key = event.key(),
            .modifiers = event.modifiers(),
            .nativeScanCode = event.nativeScanCode(),
            .nativeVirtualKey = event.nativeVirtualKey(),
            .nativeModifiers = event.nativeModifiers(),
            .text = event.text(),
            .autoRepeat = event.isAutoRepeat(),
            .count = event.count(),
        };
    }

    [[nodiscard]] QKeyEvent replay() const
    {
        return {
            pressed ? QEvent::KeyPress : QEvent::KeyRelease,
            key,
            modifiers,
            nativeScanCode,
            nativeVirtualKey,
            nativeModifiers,
            text,
            autoRepeat,
            static_cast<quint16>(std::clamp(
                count, 0,
                static_cast<int>(std::numeric_limits<quint16>::max()))),
        };
    }
};
