#pragma once

#include <QString>

#include <optional>

struct TerminfoResolution {
    QString directory;
    QString error;

    explicit operator bool() const { return !directory.isEmpty(); }
};

// Resolve the private xterm-ghostty database for an executable directory.
// A present override is authoritative: an invalid override is reported rather
// than silently falling through to a different database.
TerminfoResolution resolveTerminfoDirectory(
    const QString &executableDirectory,
    const std::optional<QString> &overrideDirectory = std::nullopt);

// Resolve using QCoreApplication::applicationDirPath() and the optional
// GHOSTTY_QT_TERMINFO environment variable.
TerminfoResolution resolveRuntimeTerminfoDirectory();
