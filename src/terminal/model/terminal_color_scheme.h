#pragma once

#include <QtGlobal>

// The terminal protocols and Ghostty's conditional configuration have only
// concrete light and dark states. Qt's Unknown value is normalized at the
// platform boundary and never crosses worker or configuration threads.
enum class TerminalColorScheme : quint8 {
    Light,
    Dark,
};
