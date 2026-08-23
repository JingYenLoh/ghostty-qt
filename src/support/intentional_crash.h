#pragma once

#include <QtGlobal>

// Keep every typed crash action on the same fatal path so the only variable
// is the thread that executes it. qFatal terminates through Qt's installed
// message handler, preserving the application's normal crash-report path.
[[noreturn]] inline void intentionalCrash(const char *threadName) noexcept
{
    qFatal("Intentional crash requested on the %s thread", threadName);
}
