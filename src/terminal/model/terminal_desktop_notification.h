#pragma once

#include <QMetaType>
#include <QString>

#include <optional>

// One parser-normalized OSC 9 or OSC 777 request. libghostty lends its UTF-8
// slices only for the callback, so this value always owns both strings before
// it crosses the session-thread boundary.
struct TerminalDesktopNotification {
    QString title;
    QString body;

    friend bool operator==(const TerminalDesktopNotification &,
                           const TerminalDesktopNotification &) = default;
};

// Returned synchronously by the worker policy and queued with other parser
// effects, preserving notification order within one terminal write.
struct TerminalCommandFinishedEffects {
    bool bell = false;
    std::optional<TerminalDesktopNotification> notification;
};

Q_DECLARE_METATYPE(TerminalDesktopNotification)
